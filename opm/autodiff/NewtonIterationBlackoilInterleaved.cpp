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

/*
  Copyright 2014 SINTEF ICT, Applied Mathematics.
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

#include <config.h>

#include <opm/autodiff/DuneMatrix.hpp>

#include <opm/autodiff/NewtonIterationBlackoilInterleaved.hpp>
#include <opm/autodiff/AutoDiffHelpers.hpp>
#include <opm/core/utility/ErrorMacros.hpp>
#include <opm/core/utility/Exceptions.hpp>
#include <opm/core/utility/Units.hpp>
#include <opm/core/linalg/LinearSolverFactory.hpp>
#include <opm/core/linalg/ParallelIstlInformation.hpp>
#include <opm/core/utility/platform_dependent/disable_warnings.h>

// #include <dune/istl/bcrsmatrix.hh>
#include <dune/istl/io.hh>
#include <dune/istl/owneroverlapcopy.hh>
#include <dune/istl/preconditioners.hh>
#include <dune/istl/schwarz.hh>
#include <dune/istl/solvers.hh>
#include <dune/istl/paamg/amg.hh>
#include <dune/istl/paamg/kamg.hh>
#include <dune/istl/paamg/pinfo.hh>

#include <opm/core/utility/platform_dependent/reenable_warnings.h>

#if HAVE_UMFPACK
#include <Eigen/UmfPackSupport>
#else
#include <Eigen/SparseLU>
#endif

// namespace Dune
// {
//     typedef Dune::FieldMatrix<double, 1, 1> MatrixBlockType;
//     typedef Dune::BCRSMatrix <MatrixBlockType>        Mat;

//     template <>
//     struct MatrixDimension<Opm::DuneMatrix> : public MatrixDimension<Mat>
//     {
//     };
// }

namespace Opm
{


    typedef AutoDiffBlock<double> ADB;
    typedef ADB::V V;
    typedef ADB::M M;
    // typedef Dune::FieldMatrix<double, 1, 1> MatrixBlockType;
    // typedef Dune::BCRSMatrix <MatrixBlockType>        Mat;


    namespace {
        /// Eliminate a variable via Schur complement.
        /// \param[in]  eqs  set of equations with Jacobians
        /// \param[in]  n    index of equation/variable to eliminate.
        /// \return          new set of equations, one smaller than eqs.
        /// Note: this method requires the eliminated variable to have the same size
        /// as the equation in the corresponding position (that also will be eliminated).
        /// It also required the jacobian block n of equation n to be diagonal.
        std::vector<ADB> eliminateVariable(const std::vector<ADB>& eqs, const int n);

        /// Recover that value of a variable previously eliminated.
        /// \param[in]  equation          previously eliminated equation.
        /// \param[in]  partial_solution  solution to the remainder system after elimination.
        /// \param[in]  n                 index of equation/variable that was eliminated.
        /// \return                       solution to complete system.
        V recoverVariable(const ADB& equation, const V& partial_solution, const int n);

        /// Form an interleaved system of equations.
        /// \param[in]       num_phases  the number of fluid phases
        /// \param[in]       eqs         the equations
        /// \param[out]      A           the resulting full system matrix
        /// \param[out]      b           the right hand side
        /// This function will deal with the first num_phases
        /// equations in eqs, and return a matrix A for the full
        /// system that has a elliptic upper left corner, if possible.
        void formInterleavedSystem(const int num_phases,
                                   const std::vector<ADB>& eqs,
                                   Eigen::SparseMatrix<double, Eigen::RowMajor>& A,
                                   V& b);

    } // anonymous namespace





    /// Construct a system solver.
    NewtonIterationBlackoilInterleaved::NewtonIterationBlackoilInterleaved(const parameter::ParameterGroup& param,
                                                                           const boost::any& parallelInformation)
      : iterations_( 0 ),
        parallelInformation_(parallelInformation),
        newton_use_gmres_( param.getDefault("newton_use_gmres", false ) ),
        linear_solver_reduction_( param.getDefault("linear_solver_reduction", 1e-2 ) ),
        linear_solver_maxiter_( param.getDefault("linear_solver_maxiter", 50 ) ),
        linear_solver_restart_( param.getDefault("linear_solver_restart", 40 ) ),
        linear_solver_verbosity_( param.getDefault("linear_solver_verbosity", 0 ))
    {
    }



    /// Solve the linear system Ax = b, with A being the
    /// combined derivative matrix of the residual and b
    /// being the residual itself.
    /// \param[in] residual   residual object containing A and b.
    /// \return               the solution x
    NewtonIterationBlackoilInterleaved::SolutionVector
    NewtonIterationBlackoilInterleaved::computeNewtonIncrement(const LinearisedBlackoilResidual& residual) const
    {
        // Build the vector of equations.
        const int np = residual.material_balance_eq.size();
        std::vector<ADB> eqs;
        eqs.reserve(np + 2);
        for (int phase = 0; phase < np; ++phase) {
            eqs.push_back(residual.material_balance_eq[phase]);
        }

        // check if wells are present
        const bool hasWells = residual.well_flux_eq.size() > 0 ;
        std::vector<ADB> elim_eqs;
        if( hasWells )
        {
            eqs.push_back(residual.well_flux_eq);
            eqs.push_back(residual.well_eq);

            // Eliminate the well-related unknowns, and corresponding equations.
            elim_eqs.reserve(2);
            elim_eqs.push_back(eqs[np]);
            eqs = eliminateVariable(eqs, np); // Eliminate well flux unknowns.
            elim_eqs.push_back(eqs[np]);
            eqs = eliminateVariable(eqs, np); // Eliminate well bhp unknowns.
            assert(int(eqs.size()) == np);
        }

        // Scale material balance equations.
        const double matbalscale[3] = { 1.1169, 1.0031, 0.0031 }; // HACK hardcoded instead of computed.
        for (int phase = 0; phase < np; ++phase) {
            eqs[phase] = eqs[phase] * matbalscale[phase];
        }

        // Find sparsity structure as union of basic block sparsity structures,
        // corresponding to the jacobians with respect to pressure.
        // Use addition to get to the union structure.
        Eigen::SparseMatrix<double> structure = eqs[0].derivative()[0];
        for (int phase = 0; phase < np; ++phase) {
            structure += eqs[phase].derivative()[0];
        }
        Eigen::SparseMatrix<double, Eigen::RowMajor> s = structure;

        // Form modified system.
        Eigen::SparseMatrix<double, Eigen::RowMajor> A;
        V b;
        formInterleavedSystem(np, eqs, A, b);

        // Create ISTL matrix.
        assert(np == 3);
        Mat istlA(s.rows(), s.cols(), s.nonZeros(), Mat::row_wise);
        const int* ia = s.outerIndexPtr();
        const int* ja = s.innerIndexPtr();
        for (Mat::CreateIterator row = istlA.createbegin(); row != istlA.createend(); ++row) {
            int ri = row.index();
            for (int i = ia[ri]; i < ia[ri + 1]; ++i) {
                row.insert(ja[i]);
            }
        }
        const int size = s.rows();
        Span span[3] = { Span(size, 1, 0),
                         Span(size, 1, size),
                         Span(size, 1, 2*size) };
        for (int row = 0; row < size; ++row) {
            for (int col_ix = ia[row]; col_ix < ia[row + 1]; ++col_ix) {
                const int col = ja[col_ix];
                MatrixBlockType block;
                for (int p1 = 0; p1 < np; ++p1) {
                    for (int p2 = 0; p2 < np; ++p2) {
                        block[p1][p2] = A.coeff(span[p1][row], span[p2][col]);
                    }
                }
                istlA[row][col] = block;
            }
        }


        // // Scale pressure equation.
        // const double pscale = 200*unit::barsa;
        // const int nc = residual.material_balance_eq[0].size();
        // A.topRows(nc) *= pscale;
        // b.topRows(nc) *= pscale;

        // Solve reduced system.
        SolutionVector dx(SolutionVector::Zero(b.size()));

        // Create ISTL matrix.
        // DuneMatrix istlA( A );

        // Create ISTL matrix for elliptic part.
        // DuneMatrix istlAe( A.topLeftCorner(nc, nc) );

        // Right hand side.
        Vector istlb(istlA.N());
        for (int i = 0; i < size; ++i) {
            istlb[i][0] = b(i);
            istlb[i][1] = b(size + i);
            istlb[i][2] = b(2*size + i);
        }

        // System solution
        Vector x(istlA.M());
        x = 0.0;

        Dune::InverseOperatorResult result;
        // Construct operator, scalar product and vectors needed.
        typedef Dune::MatrixAdapter<Mat,Vector,Vector> Operator;
        Operator opA(istlA);
        Dune::Amg::SequentialInformation info;
        constructPreconditionerAndSolve(opA, x, istlb, info, result);

        // store number of iterations
        iterations_ = result.iterations;

        // Check for failure of linear solver.
        if (!result.converged) {
            OPM_THROW(LinearSolverProblem, "Convergence failure for linear solver.");
        }

        // Copy solver output to dx.
        for (int i = 0; i < size; ++i) {
            dx(i)          = x[i][0];
            dx(size + i)   = x[i][1];
            dx(2*size + i) = x[i][2];
        }

        if( hasWells )
        {
            // Compute full solution using the eliminated equations.
            // Recovery in inverse order of elimination.
            dx = recoverVariable(elim_eqs[1], dx, np);
            dx = recoverVariable(elim_eqs[0], dx, np);
        }
        return dx;
    }

    const boost::any& NewtonIterationBlackoilInterleaved::parallelInformation() const
    {
        return parallelInformation_;
    }



    namespace
    {


        std::vector<ADB> eliminateVariable(const std::vector<ADB>& eqs, const int n)
        {
            // Check that the variable index to eliminate is within bounds.
            const int num_eq = eqs.size();
            const int num_vars = eqs[0].derivative().size();
            if (num_eq != num_vars) {
                OPM_THROW(std::logic_error, "eliminateVariable() requires the same number of variables and equations.");
            }
            if (n >= num_eq) {
                OPM_THROW(std::logic_error, "Trying to eliminate variable from too small set of equations.");
            }

            // Schur complement of (A B ; C D) wrt. D is A - B*inv(D)*C.
            // This is applied to all 2x2 block submatrices
            // The right hand side is modified accordingly. bi = bi - B * inv(D)* bn;
            // We do not explicitly compute inv(D) instead Du = C is solved

            // Extract the submatrix
            const std::vector<M>& Jn = eqs[n].derivative();

            // Use sparse LU to solve the block submatrices i.e compute inv(D)
#if HAVE_UMFPACK
            const Eigen::UmfPackLU< M > solver(Jn[n]);
#else
            const Eigen::SparseLU< M > solver(Jn[n]);
#endif
            M id(Jn[n].rows(), Jn[n].cols());
            id.setIdentity();
            const Eigen::SparseMatrix<M::Scalar, Eigen::ColMajor> Di = solver.solve(id);

            // compute inv(D)*bn for the update of the right hand side
            const Eigen::VectorXd& Dibn = solver.solve(eqs[n].value().matrix());

            std::vector<V> vals(num_eq);              // Number n will remain empty.
            std::vector<std::vector<M>> jacs(num_eq); // Number n will remain empty.
            for (int eq = 0; eq < num_eq; ++eq) {
                jacs[eq].reserve(num_eq - 1);
                const std::vector<M>& Je = eqs[eq].derivative();
                const M& B = Je[n];
                // Update right hand side.
                vals[eq] = eqs[eq].value().matrix() - B * Dibn;
            }
            for (int var = 0; var < num_eq; ++var) {
                if (var == n) {
                    continue;
                }
                // solve Du = C
                // const M u = Di * Jn[var]; // solver.solve(Jn[var]);
                M u;
                fastSparseProduct(Di, Jn[var], u); // solver.solve(Jn[var]);
                for (int eq = 0; eq < num_eq; ++eq) {
                    if (eq == n) {
                        continue;
                    }
                    const std::vector<M>& Je = eqs[eq].derivative();
                    const M& B = Je[n];

                    // Create new jacobians.
                    // Add A
                    jacs[eq].push_back(Je[var]);
                    M& J = jacs[eq].back();
                    // Subtract Bu (B*inv(D)*C)
                    M Bu;
                    fastSparseProduct(B, u, Bu);
                    J -= Bu;
                }
            }

            // Create return value.
            std::vector<ADB> retval;
            retval.reserve(num_eq - 1);
            for (int eq = 0; eq < num_eq; ++eq) {
                if (eq == n) {
                    continue;
                }
                retval.push_back(ADB::function(std::move(vals[eq]), std::move(jacs[eq])));
            }
            return retval;
        }





        V recoverVariable(const ADB& equation, const V& partial_solution, const int n)
        {
            // The equation to solve for the unknown y (to be recovered) is
            //    Cx + Dy = b
            //    Dy = (b - Cx)
            // where D is the eliminated block, C is the jacobian of
            // the eliminated equation with respect to the
            // non-eliminated unknowms, b is the right-hand side of
            // the eliminated equation, and x is the partial solution
            // of the non-eliminated unknowns.

            const M& D = equation.derivative()[n];
            // Build C.
            std::vector<M> C_jacs = equation.derivative();
            C_jacs.erase(C_jacs.begin() + n);
            V equation_value = equation.value();
            ADB eq_coll = collapseJacs(ADB::function(std::move(equation_value), std::move(C_jacs)));
            const M& C = eq_coll.derivative()[0];

            // Use sparse LU to solve the block submatrices
#if HAVE_UMFPACK
            const Eigen::UmfPackLU< M > solver(D);
#else
            const Eigen::SparseLU< M > solver(D);
#endif

            // Compute value of eliminated variable.
            const Eigen::VectorXd b = (equation.value().matrix() - C * partial_solution.matrix());
            const Eigen::VectorXd elim_var = solver.solve(b);

            // Find the relevant sizes to use when reconstructing the full solution.
            const int nelim = equation.size();
            const int npart = partial_solution.size();
            assert(C.cols() == npart);
            const int full_size = nelim + npart;
            int start = 0;
            for (int i = 0; i < n; ++i) {
                start += equation.derivative()[i].cols();
            }
            assert(start < full_size);

            // Reconstruct complete solution vector.
            V sol(full_size);
            std::copy_n(partial_solution.data(), start, sol.data());
            std::copy_n(elim_var.data(), nelim, sol.data() + start);
            std::copy_n(partial_solution.data() + start, npart - start, sol.data() + start + nelim);
            return sol;
        }




        /// Form an interleaved system of equations.
        /// \param[in]       num_phases  the number of fluid phases
        /// \param[in]       eqs         the equations
        /// \param[out]      A           the resulting full system matrix
        /// \param[out]      b           the right hand side
        /// This function will deal with the first num_phases
        /// equations in eqs, and return a matrix A for the full
        /// system that has a elliptic upper left corner, if possible.
        void formInterleavedSystem(const int num_phases,
                                   const std::vector<ADB>& eqs_in,
                                   Eigen::SparseMatrix<double, Eigen::RowMajor>& A,
                                   V& b)
        {
            if (num_phases != 3) {
                OPM_THROW(std::logic_error, "formInterleavedSystem() requires 3 phases.");
            }

#if 1
            // A concession to MRST, to obtain more similar behaviour:
            // swap the first two equations, so that oil is first, then water.
            auto eqs = eqs_in;
            eqs[0].swap(eqs[1]);
            // Characterize the material balance equations.
            const int n = eqs[0].size();
            const double ratio_limit = 0.01;
            typedef Eigen::Array<double, Eigen::Dynamic, Eigen::Dynamic> Block;
            // The l1 block indicates if the equation for a given cell and phase is
            // sufficiently strong on the diagonal.
            Block l1 = Block::Zero(n, num_phases);
            for (int phase = 0; phase < num_phases; ++phase) {
                const M& J = eqs[phase].derivative()[0];
                V dj = J.diagonal().cwiseAbs();
                V sod = V::Zero(n);
                for (int elem = 0; elem < n; ++elem) {
                    sod(elem) = J.col(elem).cwiseAbs().sum() - dj(elem);
                }
                l1.col(phase) = (dj/sod > ratio_limit).cast<double>();
            }

            // By default, replace first equation with sum of all phase equations.
            // Build helper vectors.
            V l21 = V::Zero(n);
            V l22 = V::Ones(n);
            V l31 = V::Zero(n);
            V l33 = V::Ones(n);

            // If the first phase diagonal is not strong enough, we need further treatment.
            // Then the first equation will be the sum of the remaining equations,
            // and we swap the first equation into one of their slots.
            for (int elem = 0; elem < n; ++elem) {
                if (!l1(elem, 0)) {
                    const double l12x = l1(elem, 1);
                    const double l13x = l1(elem, 2);
                    const bool allzero = (l12x + l13x == 0);
                    if (allzero) {
                        l1(elem, 0) = 1;
                    } else {
                        if (l12x >= l13x) {
                            l21(elem) = 1;
                            l22(elem) = 0;
                        } else {
                            l31(elem) = 1;
                            l33(elem) = 0;
                        }
                    }
                }
            }

            // Construct the sparse matrix L that does the swaps and sums.
            Span i1(n, 1, 0);
            Span i2(n, 1, n);
            Span i3(n, 1, 2*n);
            std::vector< Eigen::Triplet<double> > t;
            t.reserve(7*n);
            for (int ii = 0; ii < n; ++ii) {
                t.emplace_back(i1[ii], i1[ii], l1(ii));
                t.emplace_back(i1[ii], i2[ii], l1(ii+n));
                t.emplace_back(i1[ii], i3[ii], l1(ii+2*n));
                t.emplace_back(i2[ii], i1[ii], l21(ii));
                t.emplace_back(i2[ii], i2[ii], l22(ii));
                t.emplace_back(i3[ii], i1[ii], l31(ii));
                t.emplace_back(i3[ii], i3[ii], l33(ii));
            }
            M L(3*n, 3*n);
            L.setFromTriplets(t.begin(), t.end());

            // Combine in single block.
            ADB total_residual = vertcatCollapseJacs(eqs);

            // Create output as product of L with equations.
            A = L * total_residual.derivative()[0];
            b = L * total_residual.value().matrix();
#else
            ADB total_residual = vertcatCollapseJacs(eqs_in);
            A = total_residual.derivative()[0];
            b = total_residual.value().matrix();
#endif
        }

    } // anonymous namespace


} // namespace Opm

