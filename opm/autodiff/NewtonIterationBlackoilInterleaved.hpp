/*
  Copyright 2015 SINTEF ICT, Applied Mathematics.

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

#ifndef OPM_NEWTONITERATIONBLACKOILINTERLEAVED_HEADER_INCLUDED
#define OPM_NEWTONITERATIONBLACKOILINTERLEAVED_HEADER_INCLUDED

/*
  Copyright 2014 SINTEF ICT, Applied Mathematics.
  Copyright 2015 IRIS AS
  Copyright 2015 Dr. Blatt - HPC-Simulation-Software & Services
  Copyright 2015 NTNU
  Copyright 2015 Statoil AS

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
    /// solving the reduced system (after eliminating well variables)
    /// as a block-structured matrix (one block for all cell variables).
    class NewtonIterationBlackoilInterleaved : public NewtonIterationBlackoilInterface
    {
        typedef Dune::FieldVector<double, 3   > VectorBlockType;
        typedef Dune::FieldMatrix<double, 3, 3> MatrixBlockType;
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
        NewtonIterationBlackoilInterleaved(const parameter::ParameterGroup& param,
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
        void constructPreconditionerAndSolve(O& opA,
                                             Vector& x, Vector& istlb,
                                             const P& parallelInformation,
                                             Dune::InverseOperatorResult& result) const
        {
            typedef Dune::ScalarProductChooser<Vector,P,category> ScalarProductChooser;
            std::unique_ptr<typename ScalarProductChooser::ScalarProduct>
                sp(ScalarProductChooser::construct(parallelInformation));
            // Construct preconditioner.
            typedef Dune::SeqILU0<Mat,Vector,Vector> Preconditioner;
            // typedef Opm::CPRPreconditioner<Mat,Vector,Vector,P> Preconditioner;
            parallelInformation.copyOwnerToAll(istlb, istlb);
            const double relax = 1.0;
            Preconditioner precond(opA.getmat(), relax);

            // TODO: Revise when linear solvers interface opm-core is done
            // Construct linear solver.
            // GMRes solver
            if ( newton_use_gmres_ ) {
                Dune::RestartedGMResSolver<Vector> linsolve(opA, *sp, precond,
                          linear_solver_reduction_, linear_solver_restart_, linear_solver_maxiter_, linear_solver_verbosity_);
                // Solve system.
                linsolve.apply(x, istlb, result);
            }
            else { // BiCGstab solver
                Dune::BiCGSTABSolver<Vector> linsolve(opA, *sp, precond,
                          linear_solver_reduction_, linear_solver_maxiter_, linear_solver_verbosity_);
                // Solve system.
                linsolve.apply(x, istlb, result);
            }
        }

        mutable int iterations_;
        boost::any parallelInformation_;

        const bool newton_use_gmres_;
        const double linear_solver_reduction_;
        const int    linear_solver_maxiter_;
        const int    linear_solver_restart_;
        const int    linear_solver_verbosity_;
    };

} // namespace Opm


#endif // OPM_NEWTONITERATIONBLACKOILINTERLEAVED_HEADER_INCLUDED
