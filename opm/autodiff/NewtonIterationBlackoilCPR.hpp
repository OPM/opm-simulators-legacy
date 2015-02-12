/*
  Copyright 2014 SINTEF ICT, Applied Mathematics.

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef OPM_NEWTONITERATIONBLACKOILCPR_HEADER_INCLUDED
#define OPM_NEWTONITERATIONBLACKOILCPR_HEADER_INCLUDED

#include <opm/autodiff/DuneMatrix.hpp>
#include <opm/autodiff/NewtonIterationBlackoilInterface.hpp>
#include <opm/autodiff/CPRPreconditioner.hpp>
#include <opm/core/utility/parameters/ParameterGroup.hpp>
#include <opm/core/linalg/LinearSolverInterface.hpp>
#include <dune/istl/scalarproducts.hh>
#include <dune/istl/operators.hh>
#include <dune/istl/bvector.hh>
#include <memory>

namespace Opm
{

    /// This class solves the fully implicit black-oil system by
    /// applying a Constrained Pressure Residual preconditioning
    /// strategy.
    /// The approach is similar to the one described in
    /// "Preconditioning for Efficiently Applying Algebraic Multigrid
    /// in Fully Implicit Reservoir Simulations" by Gries et al (SPE 163608).
    class NewtonIterationBlackoilCPR : public NewtonIterationBlackoilInterface
    {
        typedef Dune::FieldVector<double, 1   > VectorBlockType;
        typedef Dune::FieldMatrix<double, 1, 1> MatrixBlockType;
        typedef Dune::BCRSMatrix <MatrixBlockType>        Mat;
        typedef Dune::BlockVector<VectorBlockType>        Vector;
    public:
        
        /// Construct a system solver.
        /// \param[in] param   parameters controlling the behaviour of
        ///                    the preconditioning and choice of
        ///                    linear solvers.
        ///                    Parameters:
        ///                        cpr_relax        (default 1.0) relaxation for the preconditioner
        ///                        cpr_ilu_n        (default 0) use ILU(n) for preconditioning of the linear system
        ///                        cpr_use_amg      (default false) if true, use AMG preconditioner for elliptic part
        ///                        cpr_use_bicgstab (default true)  if true, use BiCGStab (else use CG) for elliptic part
        /// \param[in] parallelInformation In the case of a parallel run
        ///                               with dune-istl the information about the parallelization.
        NewtonIterationBlackoilCPR(const parameter::ParameterGroup& param,
                                   const boost::any& parallelInformation=boost::any());

        /// Solve the system of linear equations Ax = b, with A being the
        /// combined derivative matrix of the residual and b
        /// being the residual itself.
        /// \param[in] residual   residual object containing A and b.
        /// \return               the solution x
        virtual SolutionVector computeNewtonIncrement(const LinearisedBlackoilResidual& residual) const;

        /// \copydoc NewtonIterationBlackoilInterface::iterations
        virtual int iterations () const { return iterations_; }

        /// \copydoc NewtonIterationBlackoilInterface::parallelInformation
        virtual const boost::any& parallelInformation() const;

    private:

        /// \brief construct the CPR preconditioner and the solver.
        /// \tparam P The type of the parallel information.
        /// \param parallelInformation the information about the parallelization.
        template<int category=Dune::SolverCategory::sequential, class O, class P>   
        void constructPreconditionerAndSolve(O& opA, DuneMatrix& istlAe,
                                             Vector& x, Vector& istlb,
                                             const P& parallelInformation,
                                             Dune::InverseOperatorResult& result) const
        {
            typedef Dune::ScalarProductChooser<Vector,P,category> ScalarProductChooser;
            std::unique_ptr<typename ScalarProductChooser::ScalarProduct> 
                sp(ScalarProductChooser::construct(parallelInformation));
            // Construct preconditioner.
            // typedef Dune::SeqILU0<Mat,Vector,Vector> Preconditioner;
            typedef Opm::CPRPreconditioner<Mat,Vector,Vector,P> Preconditioner;
            Preconditioner precond(opA.getmat(), istlAe, cpr_relax_, cpr_ilu_n_, cpr_use_amg_, cpr_use_bicgstab_, parallelInformation);

            // Construct linear solver.
            const double tolerance = 1e-3;
            const int maxit = 150;
            const int verbosity = 0;
            const int restart = 40;
            Dune::RestartedGMResSolver<Vector> linsolve(opA, *sp, precond, tolerance, restart, maxit, verbosity);
            
            // Solve system.
            linsolve.apply(x, istlb, result);
        }

        mutable int iterations_;
        double cpr_relax_;
        unsigned int cpr_ilu_n_;
        bool cpr_use_amg_;
        bool cpr_use_bicgstab_;
        boost::any parallelInformation_;
    };

} // namespace Opm

#endif // OPM_NEWTONITERATIONBLACKOILCPR_HEADER_INCLUDED
