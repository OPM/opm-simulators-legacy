/*
  Copyright 2014 IRIS AS

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
#ifndef OPM_ADAPTIVETIMESTEPPING_IMPL_HEADER_INCLUDED
#define OPM_ADAPTIVETIMESTEPPING_IMPL_HEADER_INCLUDED

#include <iostream>
#include <string>
#include <utility>

#include <opm/core/simulator/SimulatorTimer.hpp>
#include <opm/core/simulator/AdaptiveSimulatorTimer.hpp>
#include <opm/core/simulator/PIDTimeStepControl.hpp>

namespace Opm {

    // AdaptiveTimeStepping
    //---------------------

    AdaptiveTimeStepping::AdaptiveTimeStepping( const parameter::ParameterGroup& param )
        : timeStepControl_()
        , initial_fraction_( param.getDefault("solver.initialfraction", double(0.25) ) )
        , restart_factor_( param.getDefault("solver.restartfactor", double(0.1) ) )
        , growth_factor_( param.getDefault("solver.growthfactor", double(1.25) ) )
        , solver_restart_max_( param.getDefault("solver.restart", int(3) ) )
        , solver_verbose_( param.getDefault("solver.verbose", bool(false) ) )
        , timestep_verbose_( param.getDefault("timestep.verbose", bool(false) ) )
        , last_timestep_( -1.0 )
    {
        // valid are "pid" and "pid+iteration"
        std::string control = param.getDefault("timestep.control", std::string("pid") );

        const double tol = param.getDefault("timestep.control.tol", double(1e-3) );
        if( control == "pid" ) {
            timeStepControl_ = TimeStepControlType( new PIDTimeStepControl( tol ) );
        }
        else if ( control == "pid+iteration" )
        {
            const int iterations = param.getDefault("timestep.control.targetiteration", int(25) );
            timeStepControl_ = TimeStepControlType( new PIDAndIterationCountTimeStepControl( iterations, tol ) );
        }
        else
            OPM_THROW(std::runtime_error,"Unsupported time step control selected "<< control );

        // make sure growth factor is something reasonable
        assert( growth_factor_ >= 1.0 );
    }


    template <class Solver, class State, class WellState>
    void AdaptiveTimeStepping::
    step( const SimulatorTimer& simulatorTimer, Solver& solver, State& state, WellState& well_state )
    {
        stepImpl( simulatorTimer, solver, state, well_state );
    }

    template <class Solver, class State, class WellState>
    void AdaptiveTimeStepping::
    step( const SimulatorTimer& simulatorTimer, Solver& solver, State& state, WellState& well_state,
          OutputWriter& outputWriter )
    {
        stepImpl( simulatorTimer, solver, state, well_state, &outputWriter );
    }

    // implementation of the step method
    template <class Solver, class State, class WState>
    void AdaptiveTimeStepping::
    stepImpl( const SimulatorTimer& simulatorTimer,
              Solver& solver, State& state, WState& well_state,
              OutputWriter* outputWriter )
    {
        const double timestep = simulatorTimer.currentStepLength();

        // init last time step as a fraction of the given time step
        if( last_timestep_ < 0 ) {
            last_timestep_ = initial_fraction_ * timestep;
        }

        // create adaptive step timer with previously used sub step size
        AdaptiveSimulatorTimer substepTimer( simulatorTimer, last_timestep_ );

        // copy states in case solver has to be restarted (to be revised)
        State  last_state( state );
        WState last_well_state( well_state );

        // counter for solver restarts
        int restarts = 0;

        // sub step time loop
        while( ! substepTimer.done() )
        {
            // get current delta t
            const double dt = substepTimer.currentStepLength() ;

            // initialize time step control in case current state is needed later
            timeStepControl_->initialize( state );

            int linearIterations = -1;
            try {
                // (linearIterations < 0 means on convergence in solver)
                linearIterations = solver.step( dt, state, well_state);

                if( solver_verbose_ ) {
                    // report number of linear iterations
                    std::cout << "Overall linear iterations used: " << linearIterations << std::endl;
                }
            }
            catch (const Opm::NumericalProblem& e) {
                std::cerr << e.what() << std::endl;
                // since linearIterations is < 0 this will restart the solver
            }
            catch (const std::runtime_error& e) {
                std::cerr << e.what() << std::endl;
                // also catch linear solver not converged
            }

            // (linearIterations < 0 means no convergence in solver)
            if( linearIterations >= 0 )
            {
                // advance by current dt
                ++substepTimer;

                // compute new time step estimate
                double dtEstimate =
                    timeStepControl_->computeTimeStepSize( dt, linearIterations, state );

                // avoid time step size growth
                if( restarts > 0 ) {
                    dtEstimate = std::min( growth_factor_ * dt, dtEstimate );
                    // solver converged, reset restarts counter
                    restarts = 0;
                }

                if( timestep_verbose_ )
                {
                    std::cout << std::endl
                              <<"Substep( " << substepTimer.currentStepNum()
                                            << " ): Current time (days)         "  << unit::convert::to(substepTimer.simulationTimeElapsed(),unit::day) << std::endl
                                  << "              Current stepsize est (days) " << unit::convert::to(dtEstimate, unit::day) << std::endl;
                }

                // write data if outputWriter was provided
                if( outputWriter ) {
                    outputWriter->writeTimeStep( substepTimer, state, well_state );
                }

                // set new time step length
                substepTimer.provideTimeStepEstimate( dtEstimate );

                // update states
                last_state      = state ;
                last_well_state = well_state;

            }
            else // in case of no convergence (linearIterations < 0)
            {
                // increase restart counter
                if( restarts >= solver_restart_max_ ) {
                    OPM_THROW(Opm::NumericalProblem,"Solver failed to converge after " << restarts << " restarts.");
                }

                const double newTimeStep = restart_factor_ * dt;
                // we need to revise this
                substepTimer.provideTimeStepEstimate( newTimeStep );
                if( solver_verbose_ )
                    std::cerr << "Solver convergence failed, restarting solver with new time step ("
                              << unit::convert::to( newTimeStep, unit::day ) <<" days)." << std::endl;

                // reset states
                state      = last_state;
                well_state = last_well_state;

                ++restarts;
            }
        }


        // store last small time step for next reportStep
        last_timestep_ = substepTimer.suggestedAverage();
        if( timestep_verbose_ )
        {
            substepTimer.report( std::cout );
            std::cout << "Last suggested step size = " << unit::convert::to( last_timestep_, unit::day ) << " (days)" << std::endl;
        }

        if( ! std::isfinite( last_timestep_ ) ) { // check for NaN
            last_timestep_ = timestep;
        }
    }
}

#endif
