/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2007 Marco Bianchetti
 Copyright (C) 2007 Cristina Duminuco

 This file is part of QuantLib, a free-software/open-source library
 for financial quantitative analysts and developers - http://quantlib.org/

 QuantLib is free software: you can redistribute it and/or modify it
 under the terms of the QuantLib license.  You should have received a
 copy of the license along with this program; if not, please email
 <quantlib-dev@lists.sf.net>. The license is also available online at
 <http://quantlib.org/license.shtml>.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the license for more details.
*/

#include "utilities.hpp"
#include <ql/models/marketmodels/correlations/timehomogeneousforwardcorrelation.hpp>
#include <ql/models/marketmodels/products/multistep/multistepcoterminalswaps.hpp>
#include <ql/models/marketmodels/products/multistep/multistepcoterminalswaptions.hpp>
#include <ql/models/marketmodels/products/multistep/multistepswap.hpp>
#include <ql/models/marketmodels/products/multiproductcomposite.hpp>
#include <ql/models/marketmodels/accountingengine.hpp>
#include <ql/models/marketmodels/utilities.hpp>
#include <ql/models/marketmodels/evolvers/lognormalcotswapratepc.hpp>
#include <ql/models/marketmodels/evolvers/lognormalfwdratepc.hpp>
#include <ql/models/marketmodels/models/flatvol.hpp>
#include <ql/models/marketmodels/models/abcdvol.hpp>
#include <ql/models/marketmodels/correlations/expcorrelations.hpp>
#include <ql/models/marketmodels/browniangenerators/mtbrowniangenerator.hpp>
#include <ql/models/marketmodels/browniangenerators/sobolbrowniangenerator.hpp>
#include <ql/models/marketmodels/swapforwardmappings.hpp>
#include <ql/models/marketmodels/curvestates/coterminalswapcurvestate.hpp>
#include <ql/methods/montecarlo/genericlsregression.hpp>
#include <ql/legacy/libormarketmodels/lmlinexpcorrmodel.hpp>
#include <ql/legacy/libormarketmodels/lmextlinexpvolmodel.hpp>
#include <ql/time/schedule.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/daycounters/simpledaycounter.hpp>
#include <ql/pricingengines/blackformula.hpp>
#include <ql/pricingengines/blackcalculator.hpp>
#include <ql/utilities/dataformatters.hpp>
#include <ql/math/integrals/segmentintegral.hpp>
#include <ql/math/statistics/convergencestatistics.hpp>
#include <ql/math/functional.hpp>
#include <ql/math/statistics/sequencestatistics.hpp>
#include <sstream>

#include <float.h>

using namespace QuantLib;


#define BEGIN(x) (x+0)
#define END(x) (x+LENGTH(x))

namespace {

    Date todaysDate, startDate, endDate;
    std::vector<Time> rateTimes;
    std::vector<Real> accruals;
    Calendar calendar;
    DayCounter dayCounter;
    std::vector<Rate> todaysForwards, todaysSwaps;
    std::vector<Real> coterminalAnnuity;
    Spread displacement;
    std::vector<DiscountFactor> todaysDiscounts;
    std::vector<Volatility> volatilities, blackVols;
    Real a, b, c, d;
    Real longTermCorrelation, beta;
    Size measureOffset_;
    unsigned long seed_;
    Size paths_, trainingPaths_;
    bool printReport_ = false;

    void setup() {

        // Times
        calendar = NullCalendar();
        todaysDate = Settings::instance().evaluationDate();
        //startDate = todaysDate + 5*Years;
        endDate = todaysDate + 10*Years;
        Schedule dates(todaysDate, endDate, Period(Semiannual),
                       calendar, Following, Following, DateGeneration::Backward, false);
        rateTimes = std::vector<Time>(dates.size()-1);
        accruals = std::vector<Real>(rateTimes.size()-1);
        dayCounter = SimpleDayCounter();
        for (Size i=1; i<dates.size(); ++i)
            rateTimes[i-1] = dayCounter.yearFraction(todaysDate, dates[i]);
        for (Size i=1; i<rateTimes.size(); ++i)
            accruals[i-1] = rateTimes[i] - rateTimes[i-1];

        // Rates & displacement
        todaysForwards = std::vector<Rate>(accruals.size());
        displacement = 0.02;
        for (Size i=0; i<todaysForwards.size(); ++i) {
            todaysForwards[i] = 0.03 + 0.0010*i;
            //todaysForwards[i] = 0.04;
        }
        LMMCurveState curveState_lmm(rateTimes);
        curveState_lmm.setOnForwardRates(todaysForwards);
        todaysSwaps = curveState_lmm.coterminalSwapRates();

        // Discounts
        todaysDiscounts = std::vector<DiscountFactor>(rateTimes.size());
        todaysDiscounts[0] = 0.95;
        for (Size i=1; i<rateTimes.size(); ++i)
            todaysDiscounts[i] = todaysDiscounts[i-1] /
                (1.0+todaysForwards[i-1]*accruals[i-1]);

        // Swaption Volatilities
        Volatility mktVols[] = {0.15541283,
                                0.18719678,
                                0.20890740,
                                0.22318179,
                                0.23212717,
                                0.23731450,
                                0.23988649,
                                0.24066384,
                                0.24023111,
                                0.23900189,
                                0.23726699,
                                0.23522952,
                                0.23303022,
                                0.23076564,
                                0.22850101,
                                0.22627951,
                                0.22412881,
                                0.22206569,
                                0.22009939
        };
        a = -0.0597;
        b =  0.1677;
        c =  0.5403;
        d =  0.1710;
        volatilities = std::vector<Volatility>(todaysSwaps.size());
        blackVols = std::vector<Volatility>(todaysSwaps.size());
        for (Size i=0; i<todaysSwaps.size(); i++) {
            volatilities[i] = todaysSwaps[i]*mktVols[i]/
                (todaysSwaps[i]+displacement);
            blackVols[i]= mktVols[i];
        }

        // Cap/Floor Correlation
        longTermCorrelation = 0.5;
        beta = 0.2;
        measureOffset_ = 5;

        // Monte Carlo
        seed_ = 42;

#ifdef _DEBUG
        paths_ = 127;
        trainingPaths_ = 31;
#else
        paths_ = 32767; //262144-1; //; // 2^15-1
        trainingPaths_ = 8191; // 2^13-1
#endif
    }

    const std::shared_ptr<SequenceStatisticsInc> simulate(
                         const std::shared_ptr<MarketModelEvolver>& evolver,
                         const MarketModelMultiProduct& product) {
        Size initialNumeraire = evolver->numeraires().front();
        Real initialNumeraireValue = todaysDiscounts[initialNumeraire];

        AccountingEngine engine(evolver, product, initialNumeraireValue);
        std::shared_ptr<SequenceStatisticsInc> stats(
                          new SequenceStatisticsInc(product.numberOfProducts()));
        engine.multiplePathValues(*stats, paths_);
        return stats;
    }


    enum MarketModelType { ExponentialCorrelationFlatVolatility,
                           ExponentialCorrelationAbcdVolatility/*,
                           CalibratedMM*/
    };

    std::string marketModelTypeToString(MarketModelType type) {
        switch (type) {
          case ExponentialCorrelationFlatVolatility:
            return "Exp. Corr. Flat Vol.";
          case ExponentialCorrelationAbcdVolatility:
            return "Exp. Corr. Abcd Vol.";
            //case CalibratedMM:
            //    return "CalibratedMarketModel";
          default:
            QL_FAIL("unknown MarketModelEvolver type");
        }
    }


    std::shared_ptr<MarketModel> makeMarketModel(
                                        const EvolutionDescription& evolution,
                                        Size numberOfFactors,
                                        MarketModelType marketModelType,
                                        Spread rateBump = 0.0,
                                        Volatility volBump = 0.0) {

        std::vector<Time> fixingTimes(evolution.rateTimes());
        fixingTimes.pop_back();
        std::shared_ptr<LmVolatilityModel> volModel(new
            LmExtLinearExponentialVolModel(fixingTimes, 0.5, 0.6, 0.1, 0.1));
        std::shared_ptr<LmCorrelationModel> corrModel(new
            LmLinearExponentialCorrelationModel(evolution.numberOfRates(),
                                                longTermCorrelation, beta));
        std::vector<Rate> bumpedRates(todaysForwards.size());
        LMMCurveState curveState_lmm(rateTimes);
        curveState_lmm.setOnForwardRates(todaysForwards);
        std::vector<Rate> usedRates = curveState_lmm.coterminalSwapRates();
        std::transform(usedRates.begin(), usedRates.end(),
                       bumpedRates.begin(),
                       std::bind1st(std::plus<Rate>(), rateBump));

        std::vector<Volatility> bumpedVols(volatilities.size());
        std::transform(volatilities.begin(), volatilities.end(),
                       bumpedVols.begin(),
                       std::bind1st(std::plus<Rate>(), volBump));
        Matrix correlations = exponentialCorrelations(evolution.rateTimes(),
                                                      longTermCorrelation,
                                                      beta);
        std::shared_ptr<PiecewiseConstantCorrelation> corr(new
            TimeHomogeneousForwardCorrelation(correlations,
                                              evolution.rateTimes()));
        switch (marketModelType) {
          case ExponentialCorrelationFlatVolatility:
            return std::shared_ptr<MarketModel>(new
                FlatVol(bumpedVols,
                               corr,
                               evolution,
                               numberOfFactors,
                               bumpedRates,
                               std::vector<Spread>(bumpedRates.size(), displacement)));
          case ExponentialCorrelationAbcdVolatility:
            return std::shared_ptr<MarketModel>(new
                AbcdVol(0.0,0.0,1.0,1.0,
                               bumpedVols,
                               corr,
                               evolution,
                               numberOfFactors,
                               bumpedRates,
                               std::vector<Spread>(bumpedRates.size(), displacement)));
            //case CalibratedMM:
            //    return std::shared_ptr<MarketModel>(new
            //        CalibratedMarketModel(volModel, corrModel,
            //                              evolution,
            //                              numberOfFactors,
            //                              bumpedForwards,
            //                              displacement));
          default:
            QL_FAIL("unknown MarketModel type");
        }
    }

    enum MeasureType { ProductSuggested, Terminal,
                       MoneyMarket, MoneyMarketPlus };

    std::string measureTypeToString(MeasureType type) {
        switch (type) {
          case ProductSuggested:
            return "ProductSuggested measure";
          case Terminal:
            return "Terminal measure";
          case MoneyMarket:
            return "Money Market measure";
          case MoneyMarketPlus:
            return "Money Market Plus measure";
          default:
            QL_FAIL("unknown measure type");
        }
    }

    std::vector<Size> makeMeasure(const MarketModelMultiProduct& product,
                                  MeasureType measureType) {
        std::vector<Size> result;
        EvolutionDescription evolution(product.evolution());
        switch (measureType) {
          case ProductSuggested:
            result = product.suggestedNumeraires();
            break;
          case Terminal:
            result = terminalMeasure(evolution);
            if (!isInTerminalMeasure(evolution, result)) {
                FAIL_CHECK("\nfailure in verifying Terminal measure:\n"
                            << to_stream(result));
            }
            break;
          case MoneyMarket:
            result = moneyMarketMeasure(evolution);
            if (!isInMoneyMarketMeasure(evolution, result)) {
                FAIL_CHECK("\nfailure in verifying MoneyMarket measure:\n"
                            << to_stream(result));
            }
            break;
          case MoneyMarketPlus:
            result = moneyMarketPlusMeasure(evolution, measureOffset_);
            if (!isInMoneyMarketPlusMeasure(evolution, result, measureOffset_)) {
                FAIL_CHECK("\nfailure in verifying MoneyMarketPlus(" <<
                            measureOffset_ << ") measure:\n" <<
                            to_stream(result));
            }
            break;
          default:
            QL_FAIL("unknown measure type");
        }
        checkCompatibility(evolution, result);
        if (printReport_) {
            INFO("    " << measureTypeToString(measureType) << ": " << to_stream(result));
        }
        return result;
    }

    enum EvolverType { Ipc, Pc , NormalPc};

    std::string evolverTypeToString(EvolverType type) {
        switch (type) {
          case Ipc:
            return "iterative predictor corrector";
          case Pc:
            return "predictor corrector";
          case NormalPc:
            return "predictor corrector for normal case";
          default:
            QL_FAIL("unknown MarketModelEvolver type");
        }
    }

    std::shared_ptr<MarketModelEvolver> makeMarketModelEvolver(
                            const std::shared_ptr<MarketModel>& marketModel,
                            const std::vector<Size>& numeraires,
                            const BrownianGeneratorFactory& generatorFactory,
                            EvolverType evolverType,
                            Size initialStep = 0) {
        switch (evolverType) {
          case Pc:
            return std::shared_ptr<MarketModelEvolver>(new
                LogNormalCotSwapRatePc(marketModel, generatorFactory,
                                            numeraires,
                                            initialStep));
          default:
            QL_FAIL("unknown CoterminalSwapMarketModelEvolver type");
        }
    }

    void checkCoterminalSwapsAndSwaptions(const SequenceStatisticsInc& stats,
                                          const Rate fixedRate,
                                          const std::vector<std::shared_ptr<StrikedTypePayoff> >& displacedPayoff,
                                          const std::shared_ptr<MarketModel>, //marketModel,
                                          const std::string& config) {

        std::vector<Real> results = stats.mean();
        std::vector<Real> errors = stats.errorEstimate();
        std::vector<Real> discrepancies(todaysForwards.size());

        Size N = todaysForwards.size();

        // check Swaps
        Real maxError = QL_MIN_REAL;
        LMMCurveState curveState_lmm(rateTimes);
        curveState_lmm.setOnForwardRates(todaysForwards);

        std::vector<Real> expectedNPVs(todaysSwaps.size());
        Real errorThreshold = 0.5;
        for (Size i=0; i<N; ++i) {
            Real expectedNPV = curveState_lmm.coterminalSwapAnnuity(i, i)
                * (todaysSwaps[i]-fixedRate) * todaysDiscounts[i];
            expectedNPVs[i] = expectedNPV;
            discrepancies[i] = (results[i]-expectedNPVs[i])/errors[i];
            maxError = std::max(std::fabs(discrepancies[i]), maxError);
        }
        if (maxError > errorThreshold) {
            INFO(config);
            for (Size i=0; i<N; ++i) {
                INFO(
                              io::ordinal(i+1) << " coterminal swap NPV: "
                              << io::rate(results[i])
                              << " +- " << io::rate(errors[i])
                              << "; expected: " << io::rate(expectedNPVs[i])
                              << "; discrepancy/error = "
                              << discrepancies[N-1-i]
                              << " standard errors");
            }
            FAIL_CHECK("test failed");
        }

        // check Swaptions
        maxError = 0;
        std::vector<Rate> expectedSwaptions(N);
        for (Size i=0; i<N; ++i) {
            Real expectedSwaption =
                BlackCalculator(displacedPayoff[i],
                                todaysSwaps[i]+displacement,
                                volatilities[i]*std::sqrt(rateTimes[i]),
                                curveState_lmm.coterminalSwapAnnuity(i,i) *
                                todaysDiscounts[i]).value();
            expectedSwaptions[i] = expectedSwaption;
            discrepancies[i] = (results[N+i]-expectedSwaptions[i])/errors[N+i];
            maxError = std::max(std::fabs(discrepancies[i]), maxError);
        }
        errorThreshold = 2.0;

        if (maxError > errorThreshold) {
            INFO(config);
            for (Size i=1; i<=N; ++i) {
                INFO(
                              io::ordinal(i) << " Swaption: "
                              << io::rate(results[2*N-i])
                              << " +- " << io::rate(errors[2*N-i])
                              << "; expected: " << io::rate(expectedSwaptions[N-i])
                              << "; discrepancy/error = "
                              << io::percent(discrepancies[N-i])
                              << " standard errors");
            }
            FAIL_CHECK("test failed");
        }
    }

}


TEST_CASE("MarketModelSmm_MultiStepCoterminalSwapsAndSwaptions", "[MarketModelSmm]") {

    INFO("Testing exact repricing of "
                       "multi-step coterminal swaps and swaptions "
                       "in a lognormal coterminal swap rate market model...");

    setup();

    Real fixedRate = 0.04;

    // swaps
    std::vector<Time> swapPaymentTimes(rateTimes.begin()+1, rateTimes.end());
    MultiStepCoterminalSwaps swaps(rateTimes, accruals, accruals,
                                   swapPaymentTimes,
                                   fixedRate);
    // swaptions
    std::vector<Time> swaptionPaymentTimes(rateTimes.begin(), rateTimes.end()-1);
    std::vector<std::shared_ptr<StrikedTypePayoff> >
        displacedPayoff(todaysForwards.size()), undisplacedPayoff(todaysForwards.size());
    for (Size i=0; i<undisplacedPayoff.size(); ++i) {
        displacedPayoff[i] = std::shared_ptr<StrikedTypePayoff>(new
            PlainVanillaPayoff(Option::Call, fixedRate+displacement));

        undisplacedPayoff[i] = std::shared_ptr<StrikedTypePayoff>(new
            PlainVanillaPayoff(Option::Call, fixedRate));
    }

    MultiStepCoterminalSwaptions swaptions(rateTimes,
                                           swaptionPaymentTimes,
                                           undisplacedPayoff);
    MultiProductComposite product;
    product.add(swaps);
    product.add(swaptions);
    product.finalize();
    EvolutionDescription evolution = product.evolution();
    MarketModelType marketModels[] = {// CalibratedMM,
                                       ExponentialCorrelationFlatVolatility,
                                       ExponentialCorrelationAbcdVolatility };
    for (Size j=0; j<LENGTH(marketModels); j++) {
        Size testedFactors[] = { /*4, 8,*/ todaysForwards.size()};
        for (Size m=0; m<LENGTH(testedFactors); ++m) {
            Size factors = testedFactors[m];
            // Composite's ProductSuggested is the Terminal one
            MeasureType measures[] = { // ProductSuggested,
                                       Terminal,
                                       //MoneyMarketPlus,
                                       MoneyMarket};
            for (Size k=0; k<LENGTH(measures); k++) {
                std::vector<Size> numeraires = makeMeasure(product, measures[k]);
                std::shared_ptr<MarketModel> marketModel =
                    makeMarketModel(evolution, factors, marketModels[j]);
                EvolverType evolvers[] = { Pc /*, Ipc */};
                std::shared_ptr<MarketModelEvolver> evolver;
                Size stop = isInTerminalMeasure(evolution, numeraires) ? 0 : 1;
                for (Size i=0; i<LENGTH(evolvers)-stop; i++) {
                    for (Size n=0; n<1; n++) {
                        //MTBrownianGeneratorFactory generatorFactory(seed_);
                        SobolBrownianGeneratorFactory generatorFactory(
                                    SobolBrownianGenerator::Diagonal, seed_);
                        evolver = makeMarketModelEvolver(marketModel,
                                                         numeraires,
                                                         generatorFactory,
                                                         evolvers[i]);
                        std::ostringstream config;
                        config <<
                            marketModelTypeToString(marketModels[j]) << ", " <<
                            factors << (factors>1 ? (factors==todaysForwards.size() ? " (full) factors, " : " factors, ") : " factor,") <<
                            measureTypeToString(measures[k]) << ", " <<
                            evolverTypeToString(evolvers[i]) << ", " <<
                            "MT BGF";
                        if (printReport_)
                            INFO("    " << config.str());
                        std::shared_ptr<SequenceStatisticsInc> stats = simulate(evolver, product);
                        checkCoterminalSwapsAndSwaptions(*stats, fixedRate,
                                                         displacedPayoff, marketModel,config.str());
                    }
                }
            }
        }
    }
}

