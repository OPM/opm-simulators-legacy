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


#if HAVE_CONFIG_H
#include "config.h"
#endif // HAVE_CONFIG_H

#include <opm/polymer/SimulatorPolymer.hpp>
#include <opm/core/utility/parameters/ParameterGroup.hpp>
#include <opm/core/utility/ErrorMacros.hpp>

#include <opm/core/pressure/IncompTpfa.hpp>

#include <opm/core/grid.h>
#include <opm/core/newwells.h>
#include <opm/core/pressure/flow_bc.h>

#include <opm/core/utility/SimulatorTimer.hpp>
#include <opm/core/utility/StopWatch.hpp>
#include <opm/core/utility/writeVtkData.hpp>
#include <opm/core/utility/miscUtilities.hpp>

#include <opm/core/fluid/IncompPropertiesInterface.hpp>
#include <opm/core/fluid/RockCompressibility.hpp>

#include <opm/core/utility/ColumnExtract.hpp>
#include <opm/core/utility/Units.hpp>
#include <opm/polymer/PolymerState.hpp>
#include <opm/core/simulator/WellState.hpp>
#include <opm/polymer/TransportModelPolymer.hpp>
#include <opm/polymer/PolymerUtilities.hpp>

#include <boost/filesystem/convenience.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/lexical_cast.hpp>

#include <numeric>
#include <fstream>


namespace Opm
{



    namespace
    {
        void outputState(const UnstructuredGrid& grid,
                         const Opm::PolymerState& state,
                         const int step,
                         const std::string& output_dir);
        void outputWaterCut(const Opm::Watercut& watercut,
                            const std::string& output_dir);
        void outputWellReport(const Opm::WellReport& wellreport,
                              const std::string& output_dir);

    } // anonymous namespace



    class SimulatorPolymer::Impl
    {
    public:
        Impl(const parameter::ParameterGroup& param,
             const UnstructuredGrid& grid,
             const IncompPropertiesInterface& props,
             const PolymerProperties& poly_props,
             const RockCompressibility* rock_comp_props,
             const Wells* wells,
             const std::vector<double>& src,
             const FlowBoundaryConditions* bcs,
             LinearSolverInterface& linsolver,
             const double* gravity);

        void run(SimulatorTimer& timer,
                 PolymerState& state,
                 WellState& well_state);

    private:
        // Data.

        // Parameters for output.
        bool output_;
        std::string output_dir_;
        int output_interval_;
        // Parameters for transport solver.
        int num_transport_substeps_;
        bool use_segregation_split_;
        // Observed objects.
        const UnstructuredGrid& grid_;
        const IncompPropertiesInterface& props_;
        const PolymerProperties& poly_props_;
        const RockCompressibility* rock_comp_props_;
        const Wells* wells_;
        const std::vector<double>& src_;
        const FlowBoundaryConditions* bcs_;
        const LinearSolverInterface& linsolver_;
        const double* gravity_;
        // Solvers
        IncompTpfa psolver_;
        TransportModelPolymer tsolver_;
        // Needed by column-based gravity segregation solver.
        std::vector< std::vector<int> > columns_;
        // Misc. data
        std::vector<int> allcells_;
        PolymerInflow poly_inflow_;
    };




    SimulatorPolymer::SimulatorPolymer(const parameter::ParameterGroup& param,
                                       const UnstructuredGrid& grid,
                                       const IncompPropertiesInterface& props,
                                       const PolymerProperties& poly_props,
                                       const RockCompressibility* rock_comp_props,
                                       const Wells* wells,
                                       const std::vector<double>& src,
                                       const FlowBoundaryConditions* bcs,
                                       LinearSolverInterface& linsolver,
                                       const double* gravity)
    {
        pimpl_.reset(new Impl(param, grid, props, poly_props, rock_comp_props, wells, src, bcs, linsolver, gravity));
    }





    void SimulatorPolymer::run(SimulatorTimer& timer,
                                PolymerState& state,
                                WellState& well_state)
    {
        pimpl_->run(timer, state, well_state);
    }






    SimulatorPolymer::Impl::Impl(const parameter::ParameterGroup& param,
                                 const UnstructuredGrid& grid,
                                 const IncompPropertiesInterface& props,
                                 const PolymerProperties& poly_props,
                                 const RockCompressibility* rock_comp_props,
                                 const Wells* wells,
                                 const std::vector<double>& src,
                                 const FlowBoundaryConditions* bcs,
                                 LinearSolverInterface& linsolver,
                                 const double* gravity)
        : grid_(grid),
          props_(props),
          poly_props_(poly_props),
          rock_comp_props_(rock_comp_props),
          wells_(wells),
          src_(src),
          bcs_(bcs),
          linsolver_(linsolver),
          gravity_(gravity),
          psolver_(grid, props, rock_comp_props, linsolver,
                   param.getDefault("nl_pressure_residual_tolerance", 1e-8),
                   param.getDefault("nl_pressure_change_tolerance", 1.0),
                   param.getDefault("nl_pressure_maxiter", 10),
                   gravity, wells, src, bcs),
          tsolver_(grid, props, poly_props, TransportModelPolymer::Bracketing,
                   param.getDefault("nl_tolerance", 1e-9),
                   param.getDefault("nl_maxiter", 30)),
          poly_inflow_(param.getDefault("poly_start_days", 300.0)*Opm::unit::day,
                       param.getDefault("poly_end_days", 800.0)*Opm::unit::day,
                       param.getDefault("poly_amount", poly_props.cMax()))
    {
        // For output.
        output_ = param.getDefault("output", true);
        if (output_) {
            output_dir_ = param.getDefault("output_dir", std::string("output"));
            // Ensure that output dir exists
            boost::filesystem::path fpath(output_dir_);
            try {
                create_directories(fpath);
            }
            catch (...) {
                THROW("Creating directories failed: " << fpath);
            }
            output_interval_ = param.getDefault("output_interval", 1);
        }

        // Transport related init.
        TransportModelPolymer::SingleCellMethod method;
        std::string method_string = param.getDefault("single_cell_method", std::string("Bracketing"));
        if (method_string == "Bracketing") {
            method = Opm::TransportModelPolymer::Bracketing;
        } else if (method_string == "Newton") {
            method = Opm::TransportModelPolymer::Newton;
        } else {
            THROW("Unknown method: " << method_string);
        }
        tsolver_.setPreferredMethod(method);
        num_transport_substeps_ = param.getDefault("num_transport_substeps", 1);
        use_segregation_split_ = param.getDefault("use_segregation_split", false);
        if (gravity != 0 && use_segregation_split_){
            tsolver_.initGravity(gravity);
            extractColumn(grid_, columns_);
        }

        // Misc init.
        const int num_cells = grid.number_of_cells;
        allcells_.resize(num_cells);
        for (int cell = 0; cell < num_cells; ++cell) {
            allcells_[cell] = cell;
        }
    }




    void SimulatorPolymer::Impl::run(SimulatorTimer& timer,
                                     PolymerState& state,
                                     WellState& well_state)
    {
        std::vector<double> transport_src;

        // Initialisation.
        std::vector<double> porevol;
        if (rock_comp_props_ && rock_comp_props_->isActive()) {
            computePorevolume(grid_, props_.porosity(), *rock_comp_props_, state.pressure(), porevol);
        } else {
            computePorevolume(grid_, props_.porosity(), porevol);
        }
        const double tot_porevol_init = std::accumulate(porevol.begin(), porevol.end(), 0.0);


        // Main simulation loop.
        Opm::time::StopWatch pressure_timer;
        double ptime = 0.0;
        Opm::time::StopWatch transport_timer;
        double ttime = 0.0;
        Opm::time::StopWatch total_timer;
        total_timer.start();
        std::cout << "\n\n================    Starting main simulation loop     ===============" << std::endl;
        double init_satvol[2] = { 0.0 };
        double init_polymass = 0.0;
        double satvol[2] = { 0.0 };
        double polymass = 0.0;
        double polymass_adsorbed = 0.0;
        double injected[2] = { 0.0 };
        double produced[2] = { 0.0 };
        double polyinj = 0.0;
        double polyprod = 0.0;
        double tot_injected[2] = { 0.0 };
        double tot_produced[2] = { 0.0 };
        double tot_polyinj = 0.0;
        double tot_polyprod = 0.0;
        Opm::computeSaturatedVol(porevol, state.saturation(), init_satvol);
        std::cout << "\nInitial saturations are    " << init_satvol[0]/tot_porevol_init
                  << "    " << init_satvol[1]/tot_porevol_init << std::endl;
        Opm::Watercut watercut;
        watercut.push(0.0, 0.0, 0.0);
        Opm::WellReport wellreport;
        std::vector<double> fractional_flows;
        std::vector<double> well_resflows_phase;
        if (wells_) {
            well_resflows_phase.resize((wells_->number_of_phases)*(wells_->number_of_wells), 0.0);
            wellreport.push(props_, *wells_, state.saturation(), 0.0, well_state.bhp(), well_state.perfRates());
        }
        for (; !timer.done(); ++timer) {
            // Report timestep and (optionally) write state to disk.
            timer.report(std::cout);
            if (output_ && (timer.currentStepNum() % output_interval_ == 0)) {
                outputState(grid_, state, timer.currentStepNum(), output_dir_);
            }

            // Solve pressure.
            do {
                pressure_timer.start();
                psolver_.solve(timer.currentStepLength(), state, well_state);
                pressure_timer.stop();
                double pt = pressure_timer.secsSinceStart();
                std::cout << "Pressure solver took:  " << pt << " seconds." << std::endl;
                ptime += pt;
            } while (false);

            // Process transport sources (to include bdy terms and well flows).
            Opm::computeTransportSource(grid_, src_, state.faceflux(), 1.0,
                                        wells_, well_state.perfRates(), transport_src);

            // Find inflow rate.
            const double current_time = timer.currentTime();
            double stepsize = timer.currentStepLength();
            const double inflowc0 = poly_inflow_(current_time + 1e-5*stepsize);
            const double inflowc1 = poly_inflow_(current_time + (1.0 - 1e-5)*stepsize);
            if (inflowc0 != inflowc1) {
                std::cout << "**** Warning: polymer inflow rate changes during timestep. Using rate near start of step.";
            }
            const double inflow_c = inflowc0;

            // Solve transport.
            transport_timer.start();
            if (num_transport_substeps_ != 1) {
                stepsize /= double(num_transport_substeps_);
                std::cout << "Making " << num_transport_substeps_ << " transport substeps." << std::endl;
            }
            for (int tr_substep = 0; tr_substep < num_transport_substeps_; ++tr_substep) {
                tsolver_.solve(&state.faceflux()[0], &porevol[0], &transport_src[0], stepsize, inflow_c,
                               state.saturation(), state.concentration(), state.maxconcentration());
                Opm::computeInjectedProduced(props_, poly_props_,
                                             state.saturation(), state.concentration(), state.maxconcentration(),
                                             transport_src, timer.currentStepLength(), inflow_c,
                                             injected, produced, polyinj, polyprod);
                if (use_segregation_split_) {
                    tsolver_.solveGravity(columns_, &porevol[0], stepsize,
                                          state.saturation(), state.concentration(), state.maxconcentration());
                }
            }
            transport_timer.stop();
            double tt = transport_timer.secsSinceStart();
            std::cout << "Transport solver took: " << tt << " seconds." << std::endl;
            ttime += tt;

            // Report volume balances.
            Opm::computeSaturatedVol(porevol, state.saturation(), satvol);
            polymass = Opm::computePolymerMass(porevol, state.saturation(), state.concentration(), poly_props_.deadPoreVol());
            polymass_adsorbed = Opm::computePolymerAdsorbed(props_, poly_props_, porevol, state.maxconcentration());
            tot_injected[0] += injected[0];
            tot_injected[1] += injected[1];
            tot_produced[0] += produced[0];
            tot_produced[1] += produced[1];
            tot_polyinj += polyinj;
            tot_polyprod += polyprod;
            std::cout.precision(5);
            const int width = 18;
            std::cout << "\nVolume and polymer mass balance: "
                "   water(pv)           oil(pv)       polymer(kg)\n";
            std::cout << "    Saturated volumes:     "
                      << std::setw(width) << satvol[0]/tot_porevol_init
                      << std::setw(width) << satvol[1]/tot_porevol_init
                      << std::setw(width) << polymass << std::endl;
            std::cout << "    Adsorbed volumes:      "
                      << std::setw(width) << 0.0
                      << std::setw(width) << 0.0
                      << std::setw(width) << polymass_adsorbed << std::endl;
            std::cout << "    Injected volumes:      "
                      << std::setw(width) << injected[0]/tot_porevol_init
                      << std::setw(width) << injected[1]/tot_porevol_init
                      << std::setw(width) << polyinj << std::endl;
            std::cout << "    Produced volumes:      "
                      << std::setw(width) << produced[0]/tot_porevol_init
                      << std::setw(width) << produced[1]/tot_porevol_init
                      << std::setw(width) << polyprod << std::endl;
            std::cout << "    Total inj volumes:     "
                      << std::setw(width) << tot_injected[0]/tot_porevol_init
                      << std::setw(width) << tot_injected[1]/tot_porevol_init
                      << std::setw(width) << tot_polyinj << std::endl;
            std::cout << "    Total prod volumes:    "
                      << std::setw(width) << tot_produced[0]/tot_porevol_init
                      << std::setw(width) << tot_produced[1]/tot_porevol_init
                      << std::setw(width) << tot_polyprod << std::endl;
            std::cout << "    In-place + prod - inj: "
                      << std::setw(width) << (satvol[0] + tot_produced[0] - tot_injected[0])/tot_porevol_init
                      << std::setw(width) << (satvol[1] + tot_produced[1] - tot_injected[1])/tot_porevol_init
                      << std::setw(width) << (polymass + tot_polyprod - tot_polyinj + polymass_adsorbed) << std::endl;
            std::cout << "    Init - now - pr + inj: "
                      << std::setw(width) << (init_satvol[0] - satvol[0] - tot_produced[0] + tot_injected[0])/tot_porevol_init
                      << std::setw(width) << (init_satvol[1] - satvol[1] - tot_produced[1] + tot_injected[1])/tot_porevol_init
                      << std::setw(width) << (init_polymass - polymass - tot_polyprod + tot_polyinj - polymass_adsorbed)
                      << std::endl;
            std::cout.precision(8);

            watercut.push(timer.currentTime() + timer.currentStepLength(),
                          produced[0]/(produced[0] + produced[1]),
                          tot_produced[0]/tot_porevol_init);
            if (wells_) {
                wellreport.push(props_, *wells_, state.saturation(),
                                timer.currentTime() + timer.currentStepLength(),
                                well_state.bhp(), well_state.perfRates());
            }
        }
        total_timer.stop();

        std::cout << "\n\n================    End of simulation     ===============\n"
                  << "Total time taken: " << total_timer.secsSinceStart()
                  << "\n  Pressure time:  " << ptime
                  << "\n  Transport time: " << ttime << std::endl;

        if (output_) {
            outputState(grid_, state, timer.currentStepNum(), output_dir_);
            outputWaterCut(watercut, output_dir_);
            if (wells_) {
                outputWellReport(wellreport, output_dir_);
            }
        }
    }







    namespace
    {

        void outputState(const UnstructuredGrid& grid,
                         const Opm::PolymerState& state,
                         const int step,
                         const std::string& output_dir)
        {
            // Write data in VTK format.
            std::ostringstream vtkfilename;
            vtkfilename << output_dir << "/output-" << std::setw(3) << std::setfill('0') << step << ".vtu";
            std::ofstream vtkfile(vtkfilename.str().c_str());
            if (!vtkfile) {
                THROW("Failed to open " << vtkfilename.str());
            }
            Opm::DataMap dm;
            dm["saturation"] = &state.saturation();
            dm["pressure"] = &state.pressure();
            dm["concentration"] = &state.concentration();
            dm["cmax"] = &state.maxconcentration();
            std::vector<double> cell_velocity;
            Opm::estimateCellVelocity(grid, state.faceflux(), cell_velocity);
            dm["velocity"] = &cell_velocity;
            Opm::writeVtkData(grid, dm, vtkfile);

            // Write data (not grid) in Matlab format
            for (Opm::DataMap::const_iterator it = dm.begin(); it != dm.end(); ++it) {
                std::ostringstream fname;
                fname << output_dir << "/" << it->first << "-" << std::setw(3) << std::setfill('0') << step << ".dat";
                std::ofstream file(fname.str().c_str());
                if (!file) {
                    THROW("Failed to open " << fname.str());
                }
                const std::vector<double>& d = *(it->second);
                std::copy(d.begin(), d.end(), std::ostream_iterator<double>(file, "\n"));
            }
        }


        void outputWaterCut(const Opm::Watercut& watercut,
                            const std::string& output_dir)
        {
            // Write water cut curve.
            std::string fname = output_dir  + "/watercut.txt";
            std::ofstream os(fname.c_str());
            if (!os) {
                THROW("Failed to open " << fname);
            }
            watercut.write(os);
        }


        void outputWellReport(const Opm::WellReport& wellreport,
                              const std::string& output_dir)
        {
            // Write well report.
            std::string fname = output_dir  + "/wellreport.txt";
            std::ofstream os(fname.c_str());
            if (!os) {
                THROW("Failed to open " << fname);
            }
            wellreport.write(os);
        }


    } // anonymous namespace






} // namespace Opm
