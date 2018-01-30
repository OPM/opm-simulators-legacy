/*
  Copyright 2012 SINTEF ICT, Applied Mathematics.

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <opm/core/linalg/LinearSolverFactory.hpp>

#if HAVE_SUITESPARSE_UMFPACK_H
#include <opm/core/linalg/LinearSolverUmfpack.hpp>
#endif

#if HAVE_DUNE_ISTL
#include <opm/core/linalg/LinearSolverIstl.hpp>
#endif

#if HAVE_PETSC
#include <opm/core/linalg/LinearSolverPetsc.hpp>
#endif

#include <opm/common/utility/parameters/ParameterGroup.hpp>
#include <opm/common/ErrorMacros.hpp>
#include <string>


namespace Opm
{

    LinearSolverFactory::LinearSolverFactory()
    {
#if HAVE_SUITESPARSE_UMFPACK_H
        solver_.reset(new LinearSolverUmfpack);
#elif HAVE_DUNE_ISTL
        solver_.reset(new LinearSolverIstl);
#elif HAVE_PETSC
        solver_.reset(new LinearSolverPetsc);
#else
        OPM_THROW(std::runtime_error, "No linear solver available, you must have UMFPACK , dune-istl or Petsc installed to use LinearSolverFactory.");
#endif
    }




    LinearSolverFactory::LinearSolverFactory(const ParameterGroup& param)
    {
#if HAVE_SUITESPARSE_UMFPACK_H
        std::string default_solver = "umfpack";
#elif HAVE_DUNE_ISTL
        std::string default_solver = "istl";
#elif HAVE_PETSC
        std::string default_solver = "petsc";
#else
        std::string default_solver = "no_solver_available";
        OPM_THROW(std::runtime_error, "No linear solver available, you must have UMFPACK , dune-istl or Petsc installed to use LinearSolverFactory.");
#endif

        const std::string ls =
            param.getDefault("linsolver", default_solver);

        if (ls == "umfpack") {
#if HAVE_SUITESPARSE_UMFPACK_H
            solver_.reset(new LinearSolverUmfpack);
#endif
        }

        else if (ls == "istl") {
#if HAVE_DUNE_ISTL
            solver_.reset(new LinearSolverIstl(param));
#endif
        }
        else if (ls == "petsc"){
#if HAVE_PETSC
            solver_.reset(new LinearSolverPetsc(param));
#endif
        }

        else {
            OPM_THROW(std::runtime_error, "Linear solver " << ls << " is unknown.");
        }

        if (! solver_) {
            OPM_THROW(std::runtime_error, "Linear solver " << ls << " is not enabled in "
                  "this configuration.");
        }
    }




    LinearSolverFactory::~LinearSolverFactory()
    {
    }




    LinearSolverInterface::LinearSolverReport
    LinearSolverFactory::solve(const int size,
                               const int nonzeros,
                               const int* ia,
                               const int* ja,
                               const double* sa,
                               const double* rhs,
                               double* solution,
                               const boost::any& add) const
    {
        return solver_->solve(size, nonzeros, ia, ja, sa, rhs, solution, add);
    }

    void LinearSolverFactory::setTolerance(const double tol)
    {
        solver_->setTolerance(tol);
    }

    double LinearSolverFactory::getTolerance() const
    {
        return solver_->getTolerance();
    }



} // namespace Opm

