#include <iostream>
#include <opm/core/eclipse/EclipseGridParser.hpp>
#include <opm/core/utility/parameters/ParameterGroup.hpp>


#include "opm/core/utility/initState.hpp"
#include <opm/core/WellsManager.hpp>
#include <opm/core/GridManager.hpp>
#include <opm/core/pressure/IncompTpfa.hpp>
#include <opm/core/fluid/IncompPropertiesFromDeck.hpp>
#include <opm/core/newwells.h>
#include <opm/core/grid.h>
#include <opm/core/utility/miscUtilities.hpp>
#include <opm/core/TwophaseState.hpp>
#include <opm/core/pressure/FlowBCManager.hpp>
#include <opm/core/linalg/LinearSolverFactory.hpp>
int main(int argc, char** argv) {

    using namespace Opm::parameter;
    using namespace Opm;
    ParameterGroup parameters( argc, argv, false );
    std::string file_name = parameters.getDefault<std::string>("inputdeck", "data.data");

    // Read input file
    EclipseGridParser parser(file_name);
    std::cout << "Done!" << std::endl;
    // Setup grid
    GridManager grid(parser);
    
    // Finally handle the wells
    WellsManager wells(parser, *grid.c_grid(), NULL);
    
    std::vector<int> global_cells(grid.c_grid()->global_cell, grid.c_grid()->global_cell + grid.c_grid()->number_of_cells);

    double gravity[3] = {0.0, 0.0, parameters.getDefault<double>("gravity", 0.0)};
    IncompPropertiesFromDeck incomp_properties(parser, global_cells);

    Opm::LinearSolverFactory linsolver(parameters);

    // EXPERIMENT_ISTL
    IncompTpfa pressure_solver(*grid.c_grid(), incomp_properties.permeability(), 
                               gravity, linsolver,  wells.c_wells());
    
    
    std::vector<int> all_cells;
    for(int i = 0; i < grid.c_grid()->number_of_cells; i++) {
        all_cells.push_back(i);
    }
            
    Opm::TwophaseState state;
    
    initStateTwophaseFromDeck(*grid.c_grid(), incomp_properties, parser, gravity[2], state);
    
    // Compute phase mobilities
    std::vector<double> phase_mob;
    computePhaseMobilities(incomp_properties, all_cells, state.saturation(), phase_mob);
    // Compute total mobility and omega
    std::vector<double> totmob;
    std::vector<double> omega;
    computeTotalMobilityOmega(incomp_properties, all_cells, state.saturation(), totmob, omega);
    
    std::vector<double> wdp;
    computeWDP(*wells.c_wells(), *grid.c_grid(), state.saturation(), incomp_properties.density(), gravity[2], true, wdp);
    
    std::vector<double> src;
    Opm::FlowBCManager bcs;

    std::vector<double> pressure;
    std::vector<double> face_flux;

    std::vector<double> well_bhp;
    std::vector<double> well_rate_per_cell;
    pressure_solver.solve(totmob, omega, src, wdp, bcs.c_bcs(), pressure, face_flux, well_bhp, well_rate_per_cell);
    std::cout << "Solved" << std::endl;
    
    for(size_t i = 0; i < well_rate_per_cell.size(); i++) {
        std::cout << well_rate_per_cell[i] << std::endl;
    }
    std::vector<double> well_rate;
    
    // This will be refactored into a separate function once done.
    const int np = incomp_properties.numPhases();
    std::vector<double> fractional_flows(grid.c_grid()->number_of_cells*np, 0.0);
    for (int cell = 0; cell < grid.c_grid()->number_of_cells; ++cell) {
        double phase_sum = 0.0;
        for (int phase = 0; phase < np; ++phase) {
            phase_sum += phase_mob[cell*np + phase];
        }
        for (int phase = 0; phase < np; ++phase) {
            fractional_flows[cell*np + phase] = phase_mob[cell*np + phase] / phase_sum;
        }
    }
    // End stuff that needs to be refactored into a seperated function
    
    computeFlowRatePerWell(*wells.c_wells(), well_rate_per_cell, well_rate);
    
    // This will be refactored into a separate function once done
    std::vector<double> well_resflows(wells.c_wells()->number_of_wells*np, 0.0);
    for ( int wix = 0; wix < wells.c_wells()->number_of_wells; ++wix) {
        for (int i = wells.c_wells()->well_connpos[wix]; i < wells.c_wells()->well_connpos[wix+1]; ++i) {
            const int cell = wells.c_wells()->well_cells[i];
            for (int phase = 0; phase < np; ++phase) {
                well_resflows[wix*np + phase] += well_rate_per_cell[i]*fractional_flows[cell*np + phase];
            }
        }
    }

    // We approximate (for _testing_ that resflows = surfaceflows)
    while (!wells.conditionsMet(well_bhp, well_resflows, well_resflows)) {
        std::cout << "Conditions not met for well, trying again" << std::endl;
        pressure_solver.solve(totmob, omega, src, wdp, bcs.c_bcs(), pressure, face_flux, well_bhp, well_rate_per_cell);
        std::cout << "Solved" << std::endl;


        for (int wix = 0; wix < wells.c_wells()->number_of_wells; ++wix) {
            for (int phase = 0; phase < np; ++phase) {
                // Reset
                well_resflows[wix * np + phase] = 0.0;
            }
            for (int i = wells.c_wells()->well_connpos[wix]; i < wells.c_wells()->well_connpos[wix + 1]; ++i) {
                const int cell = wells.c_wells()->well_cells[i];
                for (int phase = 0; phase < np; ++phase) {
                    well_resflows[wix * np + phase] += well_rate_per_cell[i] * fractional_flows[cell * np + phase];
                }
            }
        }
    }

#if 0
    std::vector<double> porevol;
    computePorevolume(*grid->c_grid(), incomp_properties, porevol);
    
    

    TwophaseFluid fluid(incomp_properties);
    TransportModel  model  (fluid, *grid->c_grid(), porevol, gravity[2], true);

    TransportSolver tsolver(model);

    TransportSource* tsrc = create_transport_source(2, 2);
    double ssrc[]   = { 1.0, 0.0 };
    double ssink[]  = { 0.0, 1.0 };
    double zdummy[] = { 0.0, 0.0 };
    
    {
        int well_cell_index = 0;
        for (int well = 0; well < wells.c_wells()->number_of_wells; ++well) {
            for( int cell = wells.c_wells()->well_connpos[well]; cell < wells.c_wells()->well_connpos[well + 1]; ++cell) {
                if (well_rate_per_cell[well_cell_index] > 0.0) {
                    append_transport_source(well_cell_index, 2, 0, 
                            well_rate_per_cell[well_cell_index], ssrc, zdummy, tsrc);
                } else if (well_rate_per_cell[well_cell_index] < 0.0) {
                    append_transport_source(well_cell_index, 2, 0, 
                            well_rate_per_cell[well_cell_index], ssink, zdummy, tsrc);
                }
            }
        }
    }

    tsolver.solve(*grid->c_grid(), tsrc, stepsize, ctrl, state, linsolve, rpt);
     
    Opm::computeInjectedProduced(*props, state.saturation(), src, stepsize, injected, produced);
#endif
    return 0;
}

