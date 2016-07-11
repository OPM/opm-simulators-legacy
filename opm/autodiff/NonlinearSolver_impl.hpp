/*
  Copyright 2013, 2015 SINTEF ICT, Applied Mathematics.
  Copyright 2015 Dr. Blatt - HPC-Simulation-Software & Services
  Copyright 2015 NTNU
  Copyright 2015 IRIS AS

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

#ifndef OPM_NONLINEARSOLVER_IMPL_HEADER_INCLUDED
#define OPM_NONLINEARSOLVER_IMPL_HEADER_INCLUDED

#include <opm/autodiff/NonlinearSolver.hpp>

namespace Opm
{
    template <class PhysicalModel>
    NonlinearSolver<PhysicalModel>::NonlinearSolver(const SolverParameters& param,
                                                    std::unique_ptr<PhysicalModel> model_arg)
        : param_(param),
          model_(std::move(model_arg)),
          nonlinearIterations_(0),
          linearIterations_(0),
          wellIterations_(0),
          nonlinearIterationsLast_(0),
          linearIterationsLast_(0),
          wellIterationsLast_(0)
    {
        if (!model_) {
            OPM_THROW(std::logic_error, "Must provide a non-null model argument for NonlinearSolver.");
        }
    }

    template <class PhysicalModel>
    int NonlinearSolver<PhysicalModel>::nonlinearIterations() const
    {
        return nonlinearIterations_;
    }

    template <class PhysicalModel>
    int NonlinearSolver<PhysicalModel>::linearIterations() const
    {
        return linearIterations_;
    }

    template <class PhysicalModel>
    int NonlinearSolver<PhysicalModel>::wellIterations() const
    {
        return wellIterations_;
    }

    template <class PhysicalModel>
    const PhysicalModel& NonlinearSolver<PhysicalModel>::model() const
    {
        return *model_;
    }

    template <class PhysicalModel>
    PhysicalModel& NonlinearSolver<PhysicalModel>::model()
    {
        return *model_;
    }

    template <class PhysicalModel>
    int NonlinearSolver<PhysicalModel>::nonlinearIterationsLastStep() const
    {
        return nonlinearIterationsLast_;
    }

    template <class PhysicalModel>
    int NonlinearSolver<PhysicalModel>::linearIterationsLastStep() const
    {
        return linearIterationsLast_;
    }

    template <class PhysicalModel>
    int NonlinearSolver<PhysicalModel>::wellIterationsLastStep() const
    {
        return wellIterationsLast_;
    }


    template <class PhysicalModel>
    int
    NonlinearSolver<PhysicalModel>::
    step(const double dt,
         ReservoirState& reservoir_state,
         WellState& well_state)
    {
        return step(dt, reservoir_state, well_state, reservoir_state, well_state);
    }



    template <class PhysicalModel>
    int
    NonlinearSolver<PhysicalModel>::
    step(const double dt,
         const ReservoirState& initial_reservoir_state,
         const WellState& initial_well_state,
         ReservoirState& reservoir_state,
         WellState& well_state)
    {
        // Do model-specific once-per-step calculations.
        model_->prepareStep(dt, initial_reservoir_state, initial_well_state);

        int iteration = 0;

        // Let the model do one nonlinear iteration.

        // Set up for main solver loop.
        int linIters = 0;
        bool converged = false;
        int wellIters = 0;

        // ----------  Main nonlinear solver loop  ----------
        do {
            // Do the nonlinear step. If we are in a converged state, the
            // model will usually do an early return without an expensive
            // solve, unless the minIter() count has not been reached yet.
            IterationReport report = model_->nonlinearIteration(iteration, dt, *this, reservoir_state, well_state);
            if (report.failed) {
                OPM_THROW(Opm::NumericalProblem, "Failed to complete a nonlinear iteration.");
            }
            converged = report.converged;
            linIters += report.linear_iterations;
            if (report.well_iterations != std::numeric_limits<int>::min()) {
                wellIters += report.well_iterations;
            } else {
                wellIters = report.well_iterations;
            }
            ++iteration;
        } while ( (!converged && (iteration <= maxIter())) || (iteration <= minIter()));

        if (!converged) {
            if (model_->terminalOutputEnabled()) {
                std::cerr << "WARNING: Failed to compute converged solution in " << iteration - 1 << " iterations." << std::endl;
            }
            return -1; // -1 indicates that the solver has to be restarted
        }

        linearIterations_ += linIters;
        nonlinearIterations_ += iteration - 1; // Since the last one will always be trivial.
        wellIterations_ += wellIters; 
        linearIterationsLast_ = linIters;
        nonlinearIterationsLast_ = iteration;
        wellIterationsLast_ = wellIters;

        // Do model-specific post-step actions.
        model_->afterStep(dt, reservoir_state, well_state);

        return linIters;
    }



    template <class PhysicalModel>
    void NonlinearSolver<PhysicalModel>::SolverParameters::
    reset()
    {
        // default values for the solver parameters
        relax_type_      = DAMPEN;
        relax_max_       = 0.5;
        relax_increment_ = 0.1;
        relax_rel_tol_   = 0.2;
        max_iter_        = 15;
        min_iter_        = 1;
    }

    template <class PhysicalModel>
    NonlinearSolver<PhysicalModel>::SolverParameters::
    SolverParameters()
    {
        // set default values
        reset();
    }

    template <class PhysicalModel>
    NonlinearSolver<PhysicalModel>::SolverParameters::
    SolverParameters( const parameter::ParameterGroup& param )
    {
        // set default values
        reset();

        // overload with given parameters
        relax_max_   = param.getDefault("relax_max", relax_max_);
        max_iter_    = param.getDefault("max_iter", max_iter_);
        min_iter_    = param.getDefault("min_iter", min_iter_);

        std::string relaxation_type = param.getDefault("relax_type", std::string("dampen"));
        if (relaxation_type == "dampen") {
            relax_type_ = DAMPEN;
        } else if (relaxation_type == "sor") {
            relax_type_ = SOR;
        } else {
            OPM_THROW(std::runtime_error, "Unknown Relaxtion Type " << relaxation_type);
        }
    }

    template <class PhysicalModel>
    void
    NonlinearSolver<PhysicalModel>::detectOscillations(const std::vector<std::vector<double>>& residual_history,
                                                       const int it,
                                                       bool& oscillate, bool& stagnate) const
    {
        // The detection of oscillation in two primary variable results in the report of the detection
        // of oscillation for the solver.
        // Only the saturations are used for oscillation detection for the black oil model.
        // Stagnate is not used for any treatment here.

        if ( it < 2 ) {
            oscillate = false;
            stagnate = false;
            return;
        }

        stagnate = true;
        int oscillatePhase = 0;
        const std::vector<double>& F0 = residual_history[it];
        const std::vector<double>& F1 = residual_history[it - 1];
        const std::vector<double>& F2 = residual_history[it - 2];
        for (int p= 0; p < model_->numPhases(); ++p){
            const double d1 = std::abs((F0[p] - F2[p]) / F0[p]);
            const double d2 = std::abs((F0[p] - F1[p]) / F0[p]);

            oscillatePhase += (d1 < relaxRelTol()) && (relaxRelTol() < d2);

            // Process is 'stagnate' unless at least one phase
            // exhibits significant residual change.
            stagnate = (stagnate && !(std::abs((F1[p] - F2[p]) / F2[p]) > 1.0e-3));
        }

        oscillate = (oscillatePhase > 1);
    }


    template <class PhysicalModel>
    void
    NonlinearSolver<PhysicalModel>::stabilizeNonlinearUpdate(V& dx, V& dxOld, const double omega) const
    {
        // The dxOld is updated with dx.
        // If omega is equal to 1., no relaxtion will be appiled.

        const V tempDxOld = dxOld;
        dxOld = dx;

        switch (relaxType()) {
            case DAMPEN:
                if (omega == 1.) {
                    return;
                }
                dx = dx*omega;
                return;
            case SOR:
                if (omega == 1.) {
                    return;
                }
                dx = dx*omega + (1.-omega)*tempDxOld;
                return;
            default:
                OPM_THROW(std::runtime_error, "Can only handle DAMPEN and SOR relaxation type.");
        }

        return;
    }


} // namespace Opm


#endif // OPM_FULLYIMPLICITSOLVER_IMPL_HEADER_INCLUDED
