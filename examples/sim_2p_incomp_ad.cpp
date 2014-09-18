/*
  Copyright 2013 SINTEF ICT, Applied Mathematics.

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

#include <opm/core/pressure/FlowBCManager.hpp>

#include <opm/core/grid.h>
#include <opm/core/grid/GridManager.hpp>
#include <opm/core/wells.h>
#include <opm/core/wells/WellsManager.hpp>
#include <opm/core/utility/ErrorMacros.hpp>
#include <opm/core/simulator/initState.hpp>
#include <opm/core/simulator/SimulatorReport.hpp>
#include <opm/core/simulator/SimulatorTimer.hpp>
#include <opm/core/utility/miscUtilities.hpp>
#include <opm/core/utility/parameters/ParameterGroup.hpp>

#include <opm/core/props/IncompPropertiesBasic.hpp>
#include <opm/core/props/IncompPropertiesFromDeck.hpp>
#include <opm/core/props/rock/RockCompressibility.hpp>

#include <opm/core/linalg/LinearSolverFactory.hpp>

#include <opm/core/simulator/TwophaseState.hpp>
#include <opm/core/simulator/WellState.hpp>
#include <opm/autodiff/SimulatorIncompTwophaseAd.hpp>

#include <opm/parser/eclipse/Parser/Parser.hpp>
#include <opm/parser/eclipse/EclipseState/EclipseState.hpp>

#include <boost/filesystem.hpp>

#include <memory>
#include <algorithm>
#include <iostream>
#include <vector>
#include <numeric>


namespace
{
    void warnIfUnusedParams(const Opm::parameter::ParameterGroup& param)
    {
        if (param.anyUnused()) {
            std::cout << "--------------------   Unused parameters:   --------------------\n";
            param.displayUsage();
            std::cout << "----------------------------------------------------------------" << std::endl;
        }
    }
} // anon namespace



// ----------------- Main program -----------------
int
main(int argc, char** argv)
try
{
    using namespace Opm;

    std::cout << "\n================    Test program for incompressible two-phase flow     ===============\n\n";
    parameter::ParameterGroup param(argc, argv, false);
    std::cout << "---------------    Reading parameters     ---------------" << std::endl;

#if ! HAVE_SUITESPARSE_UMFPACK_H
    // This is an extra check to intercept a potentially invalid request for the
    // implicit transport solver as early as possible for the user.
    {
        const std::string transport_solver_type
            = param.getDefault<std::string>("transport_solver_type", "ad");
        if (transport_solver_type == "implicit") {
            OPM_THROW(std::runtime_error, "Cannot use implicit transport solver without UMFPACK. "
                  "Either reconfigure opm-core with SuiteSparse/UMFPACK support and recompile, "
                  "or use the reordering solver (transport_solver_type=reorder).");
        }
    }
#endif

    // If we have a "deck_filename", grid and props will be read from that.
    bool use_deck = param.has("deck_filename");
    Opm::ParserPtr parser(new Opm::Parser());
    Opm::DeckConstPtr deck;
    std::unique_ptr<GridManager> grid;
    std::unique_ptr<IncompPropertiesInterface> props;
    std::unique_ptr<RockCompressibility> rock_comp;
    EclipseStateConstPtr eclipseState;
    TwophaseState state;
    // bool check_well_controls = false;
    // int max_well_control_iterations = 0;
    double gravity[3] = { 0.0 };
    if (use_deck) {
        std::string deck_filename = param.get<std::string>("deck_filename");
        deck = parser->parseFile(deck_filename);
        eclipseState.reset(new EclipseState(deck));

        // Grid init
        grid.reset(new GridManager(deck));
        // Rock and fluid init
        props.reset(new IncompPropertiesFromDeck(deck, eclipseState, *grid->c_grid()));
        // check_well_controls = param.getDefault("check_well_controls", false);
        // max_well_control_iterations = param.getDefault("max_well_control_iterations", 10);
        // Rock compressibility.
        rock_comp.reset(new RockCompressibility(deck, eclipseState));
        // Gravity.
        gravity[2] = deck->hasKeyword("NOGRAV") ? 0.0 : unit::gravity;
        // Init state variables (saturation and pressure).
        if (param.has("init_saturation")) {
            initStateBasic(*grid->c_grid(), *props, param, gravity[2], state);
        } else {
            initStateFromDeck(*grid->c_grid(), *props, deck, gravity[2], state);
        }
    } else {
        // Grid init.
        const int nx = param.getDefault("nx", 100);
        const int ny = param.getDefault("ny", 100);
        const int nz = param.getDefault("nz", 1);
        const double dx = param.getDefault("dx", 1.0);
        const double dy = param.getDefault("dy", 1.0);
        const double dz = param.getDefault("dz", 1.0);
        grid.reset(new GridManager(nx, ny, nz, dx, dy, dz));
        // Rock and fluid init.
        props.reset(new IncompPropertiesBasic(param, grid->c_grid()->dimensions, grid->c_grid()->number_of_cells));
        // Rock compressibility.
        rock_comp.reset(new RockCompressibility(param));
        // Gravity.
        gravity[2] = param.getDefault("gravity", 0.0);
        // Init state variables (saturation and pressure).
        initStateBasic(*grid->c_grid(), *props, param, gravity[2], state);
    }

    // Warn if gravity but no density difference.
    bool use_gravity = (gravity[0] != 0.0 || gravity[1] != 0.0 || gravity[2] != 0.0);
    if (use_gravity) {
        if (props->density()[0] == props->density()[1]) {
            std::cout << "**** Warning: nonzero gravity, but zero density difference." << std::endl;
        }
    }
    const double *grav = use_gravity ? &gravity[0] : 0;

    // Initialising src
    int num_cells = grid->c_grid()->number_of_cells;
    std::vector<double> src(num_cells, 0.0);
    if (use_deck) {
        // Do nothing, wells will be the driving force, not source terms.
    } else {
        // Compute pore volumes, in order to enable specifying injection rate
        // terms of total pore volume.
        std::vector<double> porevol;
        if (rock_comp->isActive()) {
            computePorevolume(*grid->c_grid(), props->porosity(), *rock_comp, state.pressure(), porevol);
        } else {
            computePorevolume(*grid->c_grid(), props->porosity(), porevol);
        }
        const double tot_porevol_init = std::accumulate(porevol.begin(), porevol.end(), 0.0);
        const double default_injection = use_gravity ? 0.0 : 0.1;
        const double flow_per_sec = param.getDefault<double>("injected_porevolumes_per_day", default_injection)
            *tot_porevol_init/unit::day;
        src[0] = flow_per_sec;
        src[num_cells - 1] = -flow_per_sec;
    }

    // Boundary conditions.
    FlowBCManager bcs;
    if (param.getDefault("use_pside", false)) {
        int pside = param.get<int>("pside");
        double pside_pressure = param.get<double>("pside_pressure");
        bcs.pressureSide(*grid->c_grid(), FlowBCManager::Side(pside), pside_pressure);
    }

    // Linear solver.
    LinearSolverFactory linsolver(param);

    // Write parameters used for later reference.
    bool output = param.getDefault("output", true);
    std::ofstream epoch_os;
    std::string output_dir;
    if (output) {
        output_dir =
            param.getDefault("output_dir", std::string("output"));
        boost::filesystem::path fpath(output_dir);
        try {
            create_directories(fpath);
        }
        catch (...) {
            OPM_THROW(std::runtime_error, "Creating directories failed: " << fpath);
        }
        std::string filename = output_dir + "/epoch_timing.param";
        epoch_os.open(filename.c_str(), std::fstream::trunc | std::fstream::out);
        // open file to clean it. The file is appended to in SimulatorTwophase
        filename = output_dir + "/step_timing.param";
        std::fstream step_os(filename.c_str(), std::fstream::trunc | std::fstream::out);
        step_os.close();
        param.writeParam(output_dir + "/simulation.param");
    }


    std::cout << "\n\n================    Starting main simulation loop     ===============\n";

    SimulatorReport rep;
    if (!use_deck) {
        // Simple simulation without a deck.
        WellsManager wells; // no wells.
        SimulatorIncompTwophaseAd simulator(param,
                                            *grid->c_grid(),
                                            *props,
                                            rock_comp->isActive() ? rock_comp.get() : 0,
                                            wells,
                                            src,
                                            bcs.c_bcs(),
                                            linsolver,
                                            grav);
        SimulatorTimer simtimer;
        simtimer.init(param);
        warnIfUnusedParams(param);
        WellState well_state;
        well_state.init(0, state);
        rep = simulator.run(simtimer, state, well_state);
    } else {
        // With a deck, we may have more report steps etc.
        WellState well_state;
        Opm::TimeMapPtr timeMap(new Opm::TimeMap(deck));
        SimulatorTimer simtimer;
        for (size_t reportStepIdx = 0; reportStepIdx < timeMap->numTimesteps(); ++reportStepIdx) {
            // Report on start of report step.
            std::cout << "\n\n--------------    Starting report step " << reportStepIdx << "    --------------"
                      << "\n                  (number of steps left: "
                      << timeMap->numTimesteps() - reportStepIdx << ")\n\n" << std::flush;

            // Create new wells, well_state
            WellsManager wells(eclipseState , reportStepIdx , *grid->c_grid(), props->permeability());
            // @@@ HACK: we should really make a new well state and
            // properly transfer old well state to it every report step,
            // since number of wells may change etc.
            if (reportStepIdx == 0) {
                well_state.init(wells.c_wells(), state);
            }

            simtimer.setCurrentStepNum(reportStepIdx);

            // Create and run simulator.
            SimulatorIncompTwophaseAd simulator(param,
                                                *grid->c_grid(),
                                                *props,
                                                rock_comp->isActive() ? rock_comp.get() : 0,
                                                wells,
                                                src,
                                                bcs.c_bcs(),
                                                linsolver,
                                                grav);
            if (reportStepIdx == 0) {
                warnIfUnusedParams(param);
            }
            SimulatorReport epoch_rep = simulator.run(simtimer, state, well_state);
            if (output) {
                epoch_rep.reportParam(epoch_os);
            }
            // Update total timing report and remember step number.
            rep += epoch_rep;
        }
    }

    std::cout << "\n\n================    End of simulation     ===============\n\n";
    rep.report(std::cout);

    if (output) {
      std::string filename = output_dir + "/walltime.param";
      std::fstream tot_os(filename.c_str(),std::fstream::trunc | std::fstream::out);
      rep.reportParam(tot_os);
    }

}
catch (const std::exception &e) {
    std::cerr << "Program threw an exception: " << e.what() << "\n";
    throw;
}

