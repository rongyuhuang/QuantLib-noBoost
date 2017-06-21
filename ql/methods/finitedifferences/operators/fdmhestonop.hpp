/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2008 Andreas Gaida
 Copyright (C) 2008 Ralph Schreyer
 Copyright (C) 2008, 2014, 2015 Klaus Spanderen
 Copyright (C) 2015 Johannes Göttker-Schnetmann

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

/*! \file fdmhestonop.hpp
    \brief Heston linear operator
*/

#ifndef quantlib_fdm_heston_op_hpp
#define quantlib_fdm_heston_op_hpp

#include <ql/processes/hestonprocess.hpp>
#include <ql/methods/finitedifferences/utilities/fdmquantohelper.hpp>
#include <ql/methods/finitedifferences/operators/firstderivativeop.hpp>
#include <ql/methods/finitedifferences/operators/triplebandlinearop.hpp>
#include <ql/methods/finitedifferences/operators/ninepointlinearop.hpp>
#include <ql/methods/finitedifferences/operators/fdmlinearopcomposite.hpp>
#include <ql/termstructures/volatility/equityfx/localvoltermstructure.hpp>

namespace QuantLib {

    class FdmHestonEquityPart {
      public:
        FdmHestonEquityPart(
            const std::shared_ptr<FdmMesher>& mesher,
            const std::shared_ptr<YieldTermStructure>& rTS,
            const std::shared_ptr<YieldTermStructure>& qTS,
            const std::shared_ptr<FdmQuantoHelper>& quantoHelper,
            const std::shared_ptr<LocalVolTermStructure>& leverageFct
                = std::shared_ptr<LocalVolTermStructure>());

        void setTime(Time t1, Time t2);
        const TripleBandLinearOp& getMap() const;
        const Array& getL() const { return L_; }

      protected:
        Array getLeverageFctSlice(Time t1, Time t2) const;

        Array varianceValues_, volatilityValues_, L_;
        const FirstDerivativeOp  dxMap_;
        const TripleBandLinearOp dxxMap_;
        TripleBandLinearOp mapT_;

        const std::shared_ptr<FdmMesher> mesher_;
        const std::shared_ptr<YieldTermStructure> rTS_, qTS_;
        const std::shared_ptr<FdmQuantoHelper> quantoHelper_;
        const std::shared_ptr<LocalVolTermStructure> leverageFct_;
    };

    class FdmHestonVariancePart {
      public:
        FdmHestonVariancePart(
            const std::shared_ptr<FdmMesher>& mesher,
            const std::shared_ptr<YieldTermStructure>& rTS,
            Real sigma, Real kappa, Real theta);

        void setTime(Time t1, Time t2);
        const TripleBandLinearOp& getMap() const;

      protected:
        const TripleBandLinearOp dyMap_;
        TripleBandLinearOp mapT_;

        const std::shared_ptr<YieldTermStructure> rTS_;
    };


    class FdmHestonOp : public FdmLinearOpComposite {
      public:
        FdmHestonOp(
            const std::shared_ptr<FdmMesher>& mesher,
            const std::shared_ptr<HestonProcess>& hestonProcess,
            const std::shared_ptr<FdmQuantoHelper>& quantoHelper
                = std::shared_ptr<FdmQuantoHelper>(),
            const std::shared_ptr<LocalVolTermStructure>& leverageFct
                = std::shared_ptr<LocalVolTermStructure>());

        Size size() const;
        void setTime(Time t1, Time t2);

        Array apply(const Array& r) const;
        Array apply_mixed(const Array& r) const;

        Array apply_direction(Size direction,
                                          const Array& r) const;
        Array solve_splitting(Size direction,
                                          const Array& r, Real s) const;
        Array preconditioner(const Array& r, Real s) const;

#if !defined(QL_NO_UBLAS_SUPPORT)
        std::vector<SparseMatrix>  toMatrixDecomp() const;
#endif
      private:
        NinePointLinearOp correlationMap_;
        FdmHestonVariancePart dyMap_;
        FdmHestonEquityPart dxMap_;
        const std::shared_ptr<LocalVolTermStructure> leverageFct_;
    };
}

#endif
