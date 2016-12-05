/*
  Copyright 2013, 2015 SINTEF ICT, Applied Mathematics.
  Copyright 2014, 2015 Dr. Blatt - HPC-Simulation-Software & Services
  Copyright 2014, 2015 Statoil ASA.
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

#ifndef OPM_BLACKOILMODELBASE_IMPL_HEADER_INCLUDED
#define OPM_BLACKOILMODELBASE_IMPL_HEADER_INCLUDED

#include <opm/autodiff/BlackoilModelBase.hpp>
#include <opm/autodiff/BlackoilDetails.hpp>

#include <opm/autodiff/AutoDiffBlock.hpp>
#include <opm/autodiff/AutoDiffHelpers.hpp>
#include <opm/autodiff/GridHelpers.hpp>
#include <opm/autodiff/WellHelpers.hpp>
#include <opm/autodiff/BlackoilPropsAdInterface.hpp>
#include <opm/autodiff/GeoProps.hpp>
#include <opm/autodiff/WellDensitySegmented.hpp>
#include <opm/autodiff/VFPProperties.hpp>
#include <opm/autodiff/VFPProdProperties.hpp>
#include <opm/autodiff/VFPInjProperties.hpp>

#include <opm/core/grid.h>
#include <opm/core/linalg/LinearSolverInterface.hpp>
#include <opm/core/linalg/ParallelIstlInformation.hpp>
#include <opm/core/props/rock/RockCompressibility.hpp>
#include <opm/common/ErrorMacros.hpp>
#include <opm/common/Exceptions.hpp>
#include <opm/common/OpmLog/OpmLog.hpp>
#include <opm/parser/eclipse/Units/Units.hpp>
#include <opm/core/well_controls.h>
#include <opm/core/utility/parameters/ParameterGroup.hpp>
#include <opm/parser/eclipse/EclipseState/EclipseState.hpp>
#include <opm/parser/eclipse/EclipseState/Tables/TableManager.hpp>
#include <opm/common/data/SimulationDataContainer.hpp>

#include <dune/common/timer.hh>

#include <cassert>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <limits>
#include <vector>
#include <algorithm>
//#include <fstream>

// A debugging utility.
#define OPM_AD_DUMP(foo)                                                \
    do {                                                                \
        std::cout << "==========================================\n"     \
                  << #foo ":\n"                                         \
                  << collapseJacs(foo) << std::endl;                    \
    } while (0)

#define OPM_AD_DUMPVAL(foo)                                             \
    do {                                                                \
        std::cout << "==========================================\n"     \
                  << #foo ":\n"                                         \
                  << foo.value() << std::endl;                          \
    } while (0)

#define OPM_AD_DISKVAL(foo)                                             \
    do {                                                                \
        std::ofstream os(#foo);                                         \
        os.precision(16);                                               \
        os << foo.value() << std::endl;                                 \
    } while (0)


namespace Opm {

typedef AutoDiffBlock<double> ADB;
typedef ADB::V V;
typedef ADB::M M;
typedef Eigen::Array<double,
                     Eigen::Dynamic,
                     Eigen::Dynamic,
                     Eigen::RowMajor> DataBlock;

    template <class Grid, class WellModel, class Implementation>
    BlackoilModelBase<Grid, WellModel, Implementation>::
    BlackoilModelBase(const ModelParameters&          param,
                  const Grid&                     grid ,
                  const BlackoilPropsAdInterface& fluid,
                  const DerivedGeology&           geo  ,
                  const RockCompressibility*      rock_comp_props,
                  const WellModel&                well_model,
                  const NewtonIterationBlackoilInterface&    linsolver,
                  std::shared_ptr< const Opm::EclipseState > eclState,
                  const bool has_disgas,
                  const bool has_vapoil,
                  const bool terminal_output)
        : grid_  (grid)
        , fluid_ (fluid)
        , geo_   (geo)
        , rock_comp_props_(rock_comp_props)
        , vfp_properties_(
                    eclState->getTableManager().getVFPInjTables(),
                    eclState->getTableManager().getVFPProdTables())
        , linsolver_ (linsolver)
        , active_(detail::activePhases(fluid.phaseUsage()))
        , canph_ (detail::active2Canonical(fluid.phaseUsage()))
        , cells_ (detail::buildAllCells(Opm::AutoDiffGrid::numCells(grid)))
        , ops_   (grid, geo.nnc())
        , has_disgas_(has_disgas)
        , has_vapoil_(has_vapoil)
        , param_( param )
        , use_threshold_pressure_(false)
        , sd_    (fluid.numPhases())
        , phaseCondition_(AutoDiffGrid::numCells(grid))
        , well_model_ (well_model)
        , isRs_(V::Zero(AutoDiffGrid::numCells(grid)))
        , isRv_(V::Zero(AutoDiffGrid::numCells(grid)))
        , isSg_(V::Zero(AutoDiffGrid::numCells(grid)))
        , residual_ ( { std::vector<ADB>(fluid.numPhases(), ADB::null()),
                        ADB::null(),
                        ADB::null(),
                        { 1.1169, 1.0031, 0.0031 }, // the default magic numbers
                        false } )
        , terminal_output_ (terminal_output)
        , material_name_(0)
        , current_relaxation_(1.0)
        // only one region 0 used, which means average reservoir hydrocarbon conditions in
        // the field will be calculated.
        // TODO: more delicate implementation will be required if we want to handle different
        // FIP regions specified from the well specifications.
        , rate_converter_(fluid_, std::vector<int>(AutoDiffGrid::numCells(grid_),0))
    {
        if (active_[Water]) {
            material_name_.push_back("Water");
        }
        if (active_[Oil]) {
            material_name_.push_back("Oil");
        }
        if (active_[Gas]) {
            material_name_.push_back("Gas");
        }

        assert(numMaterials() == std::accumulate(active_.begin(), active_.end(), 0)); // Due to the material_name_ init above.

        const double gravity = detail::getGravity(geo_.gravity(), UgGridHelpers::dimensions(grid_));
        const V depth = Opm::AutoDiffGrid::cellCentroidsZToEigen(grid_);

        well_model_.init(&fluid_, &active_, &phaseCondition_, &vfp_properties_, gravity, depth);

        // TODO: put this for now to avoid modify the following code.
        // TODO: this code can be fragile.
#if HAVE_MPI
        const Wells* wells_arg = asImpl().well_model_.wellsPointer();

        if ( linsolver_.parallelInformation().type() == typeid(ParallelISTLInformation) )
        {
            const ParallelISTLInformation& info =
                boost::any_cast<const ParallelISTLInformation&>(linsolver_.parallelInformation());
            if ( terminal_output_ ) {
                // Only rank 0 does print to std::cout if terminal_output is enabled
                terminal_output_ = (info.communicator().rank()==0);
            }
            int local_number_of_wells = localWellsActive() ? wells().number_of_wells : 0;
            int global_number_of_wells = info.communicator().sum(local_number_of_wells);
            const bool wells_active = ( wells_arg && global_number_of_wells > 0 );
            wellModel().setWellsActive(wells_active);
            // Compute the global number of cells
            std::vector<int> v( Opm::AutoDiffGrid::numCells(grid_), 1);
            global_nc_ = 0;
            info.computeReduction(v, Opm::Reduction::makeGlobalSumFunctor<int>(), global_nc_);
        }else
#endif
        {
            wellModel().setWellsActive( localWellsActive() );
            global_nc_    =  Opm::AutoDiffGrid::numCells(grid_);
        }
    }


    template <class Grid, class WellModel, class Implementation>
    bool
    BlackoilModelBase<Grid, WellModel, Implementation>::
    isParallel() const
    {
#if HAVE_MPI
        if ( linsolver_.parallelInformation().type() !=
             typeid(ParallelISTLInformation) )
        {
            return false;
        }
        else
        {
            const auto& comm =boost::any_cast<const ParallelISTLInformation&>(linsolver_.parallelInformation()).communicator();
            return  comm.size() > 1;
        }
#else
        return false;
#endif
    }


    template <class Grid, class WellModel, class Implementation>
    void
    BlackoilModelBase<Grid, WellModel, Implementation>::
    prepareStep(const SimulatorTimerInterface& timer,
                const ReservoirState& reservoir_state,
                const WellState& /* well_state */)
    {
        const double dt = timer.currentStepLength();

        pvdt_ = geo_.poreVolume() / dt;
        if (active_[Gas]) {
            updatePrimalVariableFromState(reservoir_state);
        }
    }





    template <class Grid, class WellModel, class Implementation>
    template <class NonlinearSolverType>
    SimulatorReport
    BlackoilModelBase<Grid, WellModel, Implementation>::
    nonlinearIteration(const int iteration,
                       const SimulatorTimerInterface& timer,
                       NonlinearSolverType& nonlinear_solver,
                       ReservoirState& reservoir_state,
                       WellState& well_state)
    {
        SimulatorReport report;
        Dune::Timer perfTimer;

        perfTimer.start();
        const double dt = timer.currentStepLength();

        if (iteration == 0) {
            // For each iteration we store in a vector the norms of the residual of
            // the mass balance for each active phase, the well flux and the well equations.
            residual_norms_history_.clear();
            current_relaxation_ = 1.0;
            dx_old_ = V::Zero(sizeNonLinear());
        }
        try {
            report += asImpl().assemble(reservoir_state, well_state, iteration == 0);
            report.assemble_time += perfTimer.stop();
        }
        catch (...) {
            report.assemble_time += perfTimer.stop();
            throw;
        }

        report.total_linearizations = 1;
        perfTimer.reset();
        perfTimer.start();
        report.converged = asImpl().getConvergence(timer, iteration);
        residual_norms_history_.push_back(asImpl().computeResidualNorms());
        report.update_time += perfTimer.stop();

        const bool must_solve = (iteration < nonlinear_solver.minIter()) || (!report.converged);
        if (must_solve) {
            perfTimer.reset();
            perfTimer.start();
            report.total_newton_iterations = 1;

            // enable single precision for solvers when dt is smaller then maximal time step for single precision
            residual_.singlePrecision = ( dt < param_.maxSinglePrecisionTimeStep_ );

            // Compute the nonlinear update.
            V dx;
            try {
                dx = asImpl().solveJacobianSystem();
                report.linear_solve_time += perfTimer.stop();
                report.total_linear_iterations += linearIterationsLastSolve();
            }
            catch (...) {
                report.linear_solve_time += perfTimer.stop();
                report.total_linear_iterations += linearIterationsLastSolve();
                throw;
            }

            perfTimer.reset();
            perfTimer.start();

            if (param_.use_update_stabilization_) {
                // Stabilize the nonlinear update.
                bool isOscillate = false;
                bool isStagnate = false;
                nonlinear_solver.detectOscillations(residual_norms_history_, iteration, isOscillate, isStagnate);
                if (isOscillate) {
                    current_relaxation_ -= nonlinear_solver.relaxIncrement();
                    current_relaxation_ = std::max(current_relaxation_, nonlinear_solver.relaxMax());
                    if (terminalOutputEnabled()) {
                        std::string msg = " Oscillating behavior detected: Relaxation set to "
                            + std::to_string(current_relaxation_);
                        OpmLog::info(msg);
                    }
                }
                nonlinear_solver.stabilizeNonlinearUpdate(dx, dx_old_, current_relaxation_);
            }

            // Apply the update, applying model-dependent
            // limitations and chopping of the update.
            asImpl().updateState(dx, reservoir_state, well_state);
            report.update_time += perfTimer.stop();
        }

        return report;
    }





    template <class Grid, class WellModel, class Implementation>
    void
    BlackoilModelBase<Grid, WellModel, Implementation>::
    afterStep(const SimulatorTimerInterface& /*timer*/,
              ReservoirState& /* reservoir_state */,
              WellState& /* well_state */)
    {
        // Does nothing in this model.
    }





    template <class Grid, class WellModel, class Implementation>
    int
    BlackoilModelBase<Grid, WellModel, Implementation>::
    sizeNonLinear() const
    {
        return residual_.sizeNonLinear();
    }





    template <class Grid, class WellModel, class Implementation>
    int
    BlackoilModelBase<Grid, WellModel, Implementation>::
    linearIterationsLastSolve() const
    {
        return linsolver_.iterations();
    }





    template <class Grid, class WellModel, class Implementation>
    bool
    BlackoilModelBase<Grid, WellModel, Implementation>::
    terminalOutputEnabled() const
    {
        return terminal_output_;
    }





    template <class Grid, class WellModel, class Implementation>
    int
    BlackoilModelBase<Grid, WellModel, Implementation>::
    numPhases() const
    {
        return fluid_.numPhases();
    }





    template <class Grid, class WellModel, class Implementation>
    int
    BlackoilModelBase<Grid, WellModel, Implementation>::
    numMaterials() const
    {
        return material_name_.size();
    }





    template <class Grid, class WellModel, class Implementation>
    const std::string&
    BlackoilModelBase<Grid, WellModel, Implementation>::
    materialName(int material_index) const
    {
        assert(material_index < numMaterials());
        return material_name_[material_index];
    }





    template <class Grid, class WellModel, class Implementation>
    void
    BlackoilModelBase<Grid, WellModel, Implementation>::
    setThresholdPressures(const std::vector<double>& threshold_pressures)
    {
        const int num_faces = AutoDiffGrid::numFaces(grid_);
        const int num_nnc = geo_.nnc().numNNC();
        const int num_connections = num_faces + num_nnc;
        if (int(threshold_pressures.size()) != num_connections) {
            OPM_THROW(std::runtime_error, "Illegal size of threshold_pressures input ( " << threshold_pressures.size()
                      << " ), must be equal to number of faces + nncs ( " << num_faces << " + " << num_nnc << " ).");
        }
        use_threshold_pressure_ = true;
        // Map to interior faces.
        const int num_ifaces = ops_.internal_faces.size();
        threshold_pressures_by_connection_.resize(num_ifaces + num_nnc);
        for (int ii = 0; ii < num_ifaces; ++ii) {
            threshold_pressures_by_connection_[ii] = threshold_pressures[ops_.internal_faces[ii]];
        }
        // Handle NNCs
        // Note: the nnc threshold pressures is appended after the face threshold pressures
        for (int ii = 0; ii < num_nnc; ++ii) {
            threshold_pressures_by_connection_[ii + num_ifaces] = threshold_pressures[ii + num_faces];
        }

    }





    template <class Grid, class WellModel, class Implementation>
    BlackoilModelBase<Grid, WellModel, Implementation>::
    ReservoirResidualQuant::ReservoirResidualQuant()
        : accum(2, ADB::null())
        , mflux(   ADB::null())
        , b    (   ADB::null())
        , mu   (   ADB::null())
        , rho  (   ADB::null())
        , kr   (   ADB::null())
        , dh   (   ADB::null())
        , mob  (   ADB::null())
    {
    }

    template <class Grid, class WellModel, class Implementation>
    BlackoilModelBase<Grid, WellModel, Implementation>::
    SimulatorData::SimulatorData(int num_phases)
        : rq(num_phases)
        , rsSat(ADB::null())
        , rvSat(ADB::null())
        , fip()
    {
    }





    template <class Grid, class WellModel, class Implementation>
    void
    BlackoilModelBase<Grid, WellModel, Implementation>::
    makeConstantState(SolutionState& state) const
    {
        // HACK: throw away the derivatives. this may not be the most
        // performant way to do things, but it will make the state
        // automatically consistent with variableState() (and doing
        // things automatically is all the rage in this module ;)
        state.pressure = ADB::constant(state.pressure.value());
        state.temperature = ADB::constant(state.temperature.value());
        state.rs = ADB::constant(state.rs.value());
        state.rv = ADB::constant(state.rv.value());
        const int num_phases = state.saturation.size();
        for (int phaseIdx = 0; phaseIdx < num_phases; ++ phaseIdx) {
            state.saturation[phaseIdx] = ADB::constant(state.saturation[phaseIdx].value());
        }
        state.qs = ADB::constant(state.qs.value());
        state.bhp = ADB::constant(state.bhp.value());
        assert(state.canonical_phase_pressures.size() == static_cast<std::size_t>(Opm::BlackoilPhases::MaxNumPhases));
        for (int canphase = 0; canphase < Opm::BlackoilPhases::MaxNumPhases; ++canphase) {
            ADB& pp = state.canonical_phase_pressures[canphase];
            pp = ADB::constant(pp.value());
        }
    }





    template <class Grid, class WellModel, class Implementation>
    typename BlackoilModelBase<Grid, WellModel, Implementation>::SolutionState
    BlackoilModelBase<Grid, WellModel, Implementation>::
    variableState(const ReservoirState& x,
                  const WellState&     xw) const
    {
        std::vector<V> vars0 = asImpl().variableStateInitials(x, xw);
        std::vector<ADB> vars = ADB::variables(vars0);
        return asImpl().variableStateExtractVars(x, asImpl().variableStateIndices(), vars);
    }





    template <class Grid, class WellModel, class Implementation>
    std::vector<V>
    BlackoilModelBase<Grid, WellModel, Implementation>::
    variableStateInitials(const ReservoirState& x,
                          const WellState&     xw) const
    {
        assert(active_[ Oil ]);

        const int np = x.numPhases();

        std::vector<V> vars0;
        // p, Sw and Rs, Rv or Sg is used as primary depending on solution conditions
        // and bhp and Q for the wells
        vars0.reserve(np + 1);
        variableReservoirStateInitials(x, vars0);
        asImpl().wellModel().variableWellStateInitials(xw, vars0);
        return vars0;
    }





    template <class Grid, class WellModel, class Implementation>
    void
    BlackoilModelBase<Grid, WellModel, Implementation>::
    variableReservoirStateInitials(const ReservoirState& x, std::vector<V>& vars0) const
    {
        using namespace Opm::AutoDiffGrid;
        const int nc = numCells(grid_);
        const int np = x.numPhases();
        // Initial pressure.
        assert (not x.pressure().empty());
        const V p = Eigen::Map<const V>(& x.pressure()[0], nc, 1);
        vars0.push_back(p);

        // Initial saturation.
        assert (not x.saturation().empty());
        const DataBlock s = Eigen::Map<const DataBlock>(& x.saturation()[0], nc, np);
        const Opm::PhaseUsage pu = fluid_.phaseUsage();
        // We do not handle a Water/Gas situation correctly, guard against it.
        assert (active_[ Oil]);
        if (active_[ Water ]) {
            const V sw = s.col(pu.phase_pos[ Water ]);
            vars0.push_back(sw);
        }

        if (active_[ Gas ]) {
            // define new primary variable xvar depending on solution condition
            V xvar(nc);
            const V sg = s.col(pu.phase_pos[ Gas ]);
            const V rs = Eigen::Map<const V>(& x.gasoilratio()[0], x.gasoilratio().size());
            const V rv = Eigen::Map<const V>(& x.rv()[0], x.rv().size());
            xvar = isRs_*rs + isRv_*rv + isSg_*sg;
            vars0.push_back(xvar);
        }
    }





    template <class Grid, class WellModel, class Implementation>
    std::vector<int>
    BlackoilModelBase<Grid, WellModel, Implementation>::
    variableStateIndices() const
    {
        assert(active_[Oil]);
        std::vector<int> indices(5, -1);
        int next = 0;
        indices[Pressure] = next++;
        if (active_[Water]) {
            indices[Sw] = next++;
        }
        if (active_[Gas]) {
            indices[Xvar] = next++;
        }
        asImpl().wellModel().variableStateWellIndices(indices, next);
        assert(next == fluid_.numPhases() + 2);
        return indices;
    }





    template <class Grid, class WellModel, class Implementation>
    typename BlackoilModelBase<Grid, WellModel, Implementation>::SolutionState
    BlackoilModelBase<Grid, WellModel, Implementation>::
    variableStateExtractVars(const ReservoirState& x,
                             const std::vector<int>& indices,
                             std::vector<ADB>& vars) const
    {
        //using namespace Opm::AutoDiffGrid;
        const int nc = Opm::AutoDiffGrid::numCells(grid_);
        const Opm::PhaseUsage pu = fluid_.phaseUsage();

        SolutionState state(fluid_.numPhases());

        // Pressure.
        state.pressure = std::move(vars[indices[Pressure]]);

        // Temperature cannot be a variable at this time (only constant).
        const V temp = Eigen::Map<const V>(& x.temperature()[0], x.temperature().size());
        state.temperature = ADB::constant(temp);

        // Saturations
        {
            ADB so = ADB::constant(V::Ones(nc, 1));

            if (active_[ Water ]) {
                state.saturation[pu.phase_pos[ Water ]] = std::move(vars[indices[Sw]]);
                const ADB& sw = state.saturation[pu.phase_pos[ Water ]];
                so -= sw;
            }

            if (active_[ Gas ]) {
                // Define Sg Rs and Rv in terms of xvar.
                // Xvar is only defined if gas phase is active
                const ADB& xvar = vars[indices[Xvar]];
                ADB& sg = state.saturation[ pu.phase_pos[ Gas ] ];
                sg = isSg_*xvar + isRv_*so;
                so -= sg;

                //Compute the phase pressures before computing RS/RV
                {
                    const ADB& sw = (active_[ Water ]
                                             ? state.saturation[ pu.phase_pos[ Water ] ]
                                             : ADB::null());
                    state.canonical_phase_pressures = computePressures(state.pressure, sw, so, sg);
                }

                if (active_[ Oil ]) {
                    // RS and RV is only defined if both oil and gas phase are active.
                    sd_.rsSat = fluidRsSat(state.canonical_phase_pressures[ Oil ], so , cells_);
                    if (has_disgas_) {
                        state.rs = (1-isRs_)*sd_.rsSat + isRs_*xvar;
                    } else {
                        state.rs = sd_.rsSat;
                    }
                    sd_.rvSat = fluidRvSat(state.canonical_phase_pressures[ Gas ], so , cells_);
                    if (has_vapoil_) {
                        state.rv = (1-isRv_)*sd_.rvSat + isRv_*xvar;
                    } else {
                        state.rv = sd_.rvSat;
                    }
                }
            }
            else {
                // Compute phase pressures also if gas phase is not active
                const ADB& sw = (active_[ Water ]
                                         ? state.saturation[ pu.phase_pos[ Water ] ]
                                         : ADB::null());
                const ADB& sg = ADB::null();
                state.canonical_phase_pressures = computePressures(state.pressure, sw, so, sg);
            }

            if (active_[ Oil ]) {
                // Note that so is never a primary variable.
                state.saturation[pu.phase_pos[ Oil ]] = std::move(so);
            }
        }
        // wells
        asImpl().wellModel().variableStateExtractWellsVars(indices, vars, state);
        return state;
    }





    template <class Grid, class WellModel, class Implementation>
    void
    BlackoilModelBase<Grid, WellModel, Implementation>::
    computeAccum(const SolutionState& state,
                 const int            aix  )
    {
        const Opm::PhaseUsage& pu = fluid_.phaseUsage();

        const ADB&              press = state.pressure;
        const ADB&              temp  = state.temperature;
        const std::vector<ADB>& sat   = state.saturation;
        const ADB&              rs    = state.rs;
        const ADB&              rv    = state.rv;

        const std::vector<PhasePresence> cond = phaseCondition();

        const ADB pv_mult = poroMult(press);

        const int maxnp = Opm::BlackoilPhases::MaxNumPhases;
        for (int phase = 0; phase < maxnp; ++phase) {
            if (active_[ phase ]) {
                const int pos = pu.phase_pos[ phase ];
                sd_.rq[pos].b = asImpl().fluidReciprocFVF(phase, state.canonical_phase_pressures[phase], temp, rs, rv, cond);
                sd_.rq[pos].accum[aix] = pv_mult * sd_.rq[pos].b * sat[pos];
                // OPM_AD_DUMP(sd_.rq[pos].b);
                // OPM_AD_DUMP(sd_.rq[pos].accum[aix]);
            }
        }

        if (active_[ Oil ] && active_[ Gas ]) {
            // Account for gas dissolved in oil and vaporized oil
            const int po = pu.phase_pos[ Oil ];
            const int pg = pu.phase_pos[ Gas ];

            // Temporary copy to avoid contribution of dissolved gas in the vaporized oil
            // when both dissolved gas and vaporized oil are present.
            const ADB accum_gas_copy =sd_.rq[pg].accum[aix];

            sd_.rq[pg].accum[aix] += state.rs * sd_.rq[po].accum[aix];
            sd_.rq[po].accum[aix] += state.rv * accum_gas_copy;
            // OPM_AD_DUMP(sd_.rq[pg].accum[aix]);
        }
    }





    template <class Grid, class WellModel, class Implementation>
    SimulatorReport
    BlackoilModelBase<Grid, WellModel, Implementation>::
    assemble(const ReservoirState& reservoir_state,
             WellState& well_state,
             const bool initial_assembly)
    {
        using namespace Opm::AutoDiffGrid;

        SimulatorReport report;

        // If we have VFP tables, we need the well connection
        // pressures for the "simple" hydrostatic correction
        // between well depth and vfp table depth.
        if (isVFPActive()) {
            SolutionState state = asImpl().variableState(reservoir_state, well_state);
            SolutionState state0 = state;
            asImpl().makeConstantState(state0);
            asImpl().wellModel().computeWellConnectionPressures(state0, well_state);
        }

        // Possibly switch well controls and updating well state to
        // get reasonable initial conditions for the wells
        asImpl().wellModel().updateWellControls(well_state);

        if (asImpl().wellModel().wellCollection()->groupControlActive()) {
            // enforce VREP control when necessary.
            applyVREPGroupControl(reservoir_state, well_state);

            asImpl().wellModel().wellCollection()->updateWellTargets(well_state.wellRates());
        }

        // Create the primary variables.
        SolutionState state = asImpl().variableState(reservoir_state, well_state);

        if (initial_assembly) {
            // Create the (constant, derivativeless) initial state.
            SolutionState state0 = state;
            asImpl().makeConstantState(state0);
            // Compute initial accumulation contributions
            // and well connection pressures.
            asImpl().computeAccum(state0, 0);
            asImpl().wellModel().computeWellConnectionPressures(state0, well_state);
        }

        // OPM_AD_DISKVAL(state.pressure);
        // OPM_AD_DISKVAL(state.saturation[0]);
        // OPM_AD_DISKVAL(state.saturation[1]);
        // OPM_AD_DISKVAL(state.saturation[2]);
        // OPM_AD_DISKVAL(state.rs);
        // OPM_AD_DISKVAL(state.rv);
        // OPM_AD_DISKVAL(state.qs);
        // OPM_AD_DISKVAL(state.bhp);

        // -------- Mass balance equations --------
        asImpl().assembleMassBalanceEq(state);

        // -------- Well equations ----------
        if ( ! wellsActive() ) {
            return report;
        }

        std::vector<ADB> mob_perfcells;
        std::vector<ADB> b_perfcells;
        asImpl().wellModel().extractWellPerfProperties(state, sd_.rq, mob_perfcells, b_perfcells);
        if (param_.solve_welleq_initially_ && initial_assembly) {
            // solve the well equations as a pre-processing step
            report += asImpl().solveWellEq(mob_perfcells, b_perfcells, reservoir_state, state, well_state);
        }
        V aliveWells;
        std::vector<ADB> cq_s;
        asImpl().wellModel().computeWellFlux(state, mob_perfcells, b_perfcells, aliveWells, cq_s);
        asImpl().wellModel().updatePerfPhaseRatesAndPressures(cq_s, state, well_state);
        asImpl().wellModel().addWellFluxEq(cq_s, state, residual_);
        asImpl().addWellContributionToMassBalanceEq(cq_s, state, well_state);
        asImpl().wellModel().addWellControlEq(state, well_state, aliveWells, residual_);

        if (param_.compute_well_potentials_) {
            SolutionState state0 = state;
            asImpl().makeConstantState(state0);
            asImpl().wellModel().computeWellPotentials(mob_perfcells, b_perfcells, state0, well_state);
        }

        return report;
    }




    template <class Grid, class WellModel, class Implementation>
    void
    BlackoilModelBase<Grid, WellModel, Implementation>::
    assembleMassBalanceEq(const SolutionState& state)
    {
        // Compute b_p and the accumulation term b_p*s_p for each phase,
        // except gas. For gas, we compute b_g*s_g + Rs*b_o*s_o.
        // These quantities are stored in sd_.rq[phase].accum[1].
        // The corresponding accumulation terms from the start of
        // the timestep (b^0_p*s^0_p etc.) were already computed
        // on the initial call to assemble() and stored in sd_.rq[phase].accum[0].
        asImpl().computeAccum(state, 1);

        // Set up the common parts of the mass balance equations
        // for each active phase.
        const V transi = subset(geo_.transmissibility(), ops_.internal_faces);
        const V trans_nnc = ops_.nnc_trans;
        V trans_all(transi.size() + trans_nnc.size());
        trans_all << transi, trans_nnc;


        {
            const std::vector<ADB> kr = asImpl().computeRelPerm(state);
            for (int phaseIdx = 0; phaseIdx < fluid_.numPhases(); ++phaseIdx) {
                sd_.rq[phaseIdx].kr = kr[canph_[phaseIdx]];
            }
        }
#pragma omp parallel for schedule(static)
        for (int phaseIdx = 0; phaseIdx < fluid_.numPhases(); ++phaseIdx) {
            const std::vector<PhasePresence>& cond = phaseCondition();
            sd_.rq[phaseIdx].mu = asImpl().fluidViscosity(canph_[phaseIdx], state.canonical_phase_pressures[canph_[phaseIdx]], state.temperature, state.rs, state.rv, cond);
            sd_.rq[phaseIdx].rho = asImpl().fluidDensity(canph_[phaseIdx], sd_.rq[phaseIdx].b, state.rs, state.rv);
            asImpl().computeMassFlux(phaseIdx, trans_all, sd_.rq[phaseIdx].kr, sd_.rq[phaseIdx].mu, sd_.rq[phaseIdx].rho, state.canonical_phase_pressures[canph_[phaseIdx]], state);

            residual_.material_balance_eq[ phaseIdx ] =
                pvdt_ * (sd_.rq[phaseIdx].accum[1] - sd_.rq[phaseIdx].accum[0])
                + ops_.div*sd_.rq[phaseIdx].mflux;
        }

        // -------- Extra (optional) rs and rv contributions to the mass balance equations --------

        // Add the extra (flux) terms to the mass balance equations
        // From gas dissolved in the oil phase (rs) and oil vaporized in the gas phase (rv)
        // The extra terms in the accumulation part of the equation are already handled.
        if (active_[ Oil ] && active_[ Gas ]) {
            const int po = fluid_.phaseUsage().phase_pos[ Oil ];
            const int pg = fluid_.phaseUsage().phase_pos[ Gas ];

            const UpwindSelector<double> upwindOil(grid_, ops_,
                                                sd_.rq[po].dh.value());
            const ADB rs_face = upwindOil.select(state.rs);

            const UpwindSelector<double> upwindGas(grid_, ops_,
                                                sd_.rq[pg].dh.value());
            const ADB rv_face = upwindGas.select(state.rv);

            residual_.material_balance_eq[ pg ] += ops_.div * (rs_face * sd_.rq[po].mflux);
            residual_.material_balance_eq[ po ] += ops_.div * (rv_face * sd_.rq[pg].mflux);

            // OPM_AD_DUMP(residual_.material_balance_eq[ Gas ]);

        }


        if (param_.update_equations_scaling_) {
            asImpl().updateEquationsScaling();
        }

    }





    template <class Grid, class WellModel, class Implementation>
    void
    BlackoilModelBase<Grid, WellModel, Implementation>::
    updateEquationsScaling() {
        ADB::V B;
        const Opm::PhaseUsage& pu = fluid_.phaseUsage();
        for ( int idx=0; idx<MaxNumPhases; ++idx )
        {
            if (active_[idx]) {
                const int pos    = pu.phase_pos[idx];
                const ADB& temp_b = sd_.rq[pos].b;
                B = 1. / temp_b.value();
#if HAVE_MPI
                if ( linsolver_.parallelInformation().type() == typeid(ParallelISTLInformation) )
                {
                    const ParallelISTLInformation& real_info =
                        boost::any_cast<const ParallelISTLInformation&>(linsolver_.parallelInformation());
                    double B_global_sum = 0;
                    real_info.computeReduction(B, Reduction::makeGlobalSumFunctor<double>(), B_global_sum);
                    residual_.matbalscale[idx] = B_global_sum / global_nc_;
                }
                else
#endif
                {
                    residual_.matbalscale[idx] = B.mean();
                }
            }
        }
    }





    template <class Grid, class WellModel, class Implementation>
    void
    BlackoilModelBase<Grid, WellModel, Implementation>::
    addWellContributionToMassBalanceEq(const std::vector<ADB>& cq_s,
                                       const SolutionState&,
                                       const WellState&)
    {
        if ( !asImpl().localWellsActive() )
        {
            // If there are no wells in the subdomain of the proces then
            // cq_s has zero size and will cause a segmentation fault below.
            return;
        }

        // Add well contributions to mass balance equations
        const int nc = Opm::AutoDiffGrid::numCells(grid_);
        const int np = asImpl().numPhases();
        const V& efficiency_factors = wellModel().wellPerfEfficiencyFactors();
        for (int phase = 0; phase < np; ++phase) {
            residual_.material_balance_eq[phase] -= superset(efficiency_factors * cq_s[phase],
                                                             wellModel().wellOps().well_cells, nc);
        }
    }





    template <class Grid, class WellModel, class Implementation>
    bool
    BlackoilModelBase<Grid, WellModel, Implementation>::
    isVFPActive() const
    {
        if( ! localWellsActive() ) {
            return false;
        }

        if ( vfp_properties_.getProd()->empty() && vfp_properties_.getInj()->empty() ) {
            return false;
        }

        const int nw = wells().number_of_wells;
        //Loop over all wells
        for (int w = 0; w < nw; ++w) {
            const WellControls* wc = wells().ctrls[w];

            const int nwc = well_controls_get_num(wc);

            //Loop over all controls
            for (int c=0; c < nwc; ++c) {
                const WellControlType ctrl_type = well_controls_iget_type(wc, c);

                if (ctrl_type == THP) {
                    return true;
                }
            }
        }

        return false;
    }




    template <class Grid, class WellModel, class Implementation>
    SimulatorReport
    BlackoilModelBase<Grid, WellModel, Implementation>::
    solveWellEq(const std::vector<ADB>& mob_perfcells,
                const std::vector<ADB>& b_perfcells,
                const ReservoirState& reservoir_state,
                SolutionState& state,
                WellState& well_state)
    {
        V aliveWells;
        const int np = wells().number_of_phases;
        std::vector<ADB> cq_s(np, ADB::null());
        std::vector<int> indices = asImpl().wellModel().variableWellStateIndices();
        SolutionState state0 = state;
        WellState well_state0 = well_state;
        asImpl().makeConstantState(state0);

        std::vector<ADB> mob_perfcells_const(np, ADB::null());
        std::vector<ADB> b_perfcells_const(np, ADB::null());

        if (asImpl().localWellsActive() ){
            // If there are non well in the sudomain of the process
            // thene mob_perfcells_const and b_perfcells_const would be empty
            for (int phase = 0; phase < np; ++phase) {
                mob_perfcells_const[phase] = ADB::constant(mob_perfcells[phase].value());
                b_perfcells_const[phase] = ADB::constant(b_perfcells[phase].value());
            }
        }

        int it  = 0;
        bool converged;
        do {
            // bhp and Q for the wells
            std::vector<V> vars0;
            vars0.reserve(2);
            asImpl().wellModel().variableWellStateInitials(well_state, vars0);
            std::vector<ADB> vars = ADB::variables(vars0);

            SolutionState wellSolutionState = state0;
            asImpl().wellModel().variableStateExtractWellsVars(indices, vars, wellSolutionState);
            asImpl().wellModel().computeWellFlux(wellSolutionState, mob_perfcells_const, b_perfcells_const, aliveWells, cq_s);
            asImpl().wellModel().updatePerfPhaseRatesAndPressures(cq_s, wellSolutionState, well_state);
            asImpl().wellModel().addWellFluxEq(cq_s, wellSolutionState, residual_);
            asImpl().wellModel().addWellControlEq(wellSolutionState, well_state, aliveWells, residual_);
            converged = getWellConvergence(it);

            if (converged) {
                break;
            }

            ++it;
            if( localWellsActive() )
            {
                std::vector<ADB> eqs;
                eqs.reserve(2);
                eqs.push_back(residual_.well_flux_eq);
                eqs.push_back(residual_.well_eq);
                ADB total_residual = vertcatCollapseJacs(eqs);
                const std::vector<M>& Jn = total_residual.derivative();
                typedef Eigen::SparseMatrix<double> Sp;
                Sp Jn0;
                Jn[0].toSparse(Jn0);
                const Eigen::SparseLU< Sp > solver(Jn0);
                ADB::V total_residual_v = total_residual.value();
                const Eigen::VectorXd& dx = solver.solve(total_residual_v.matrix());
                assert(dx.size() == total_residual_v.size());
                asImpl().wellModel().updateWellState(dx.array(), dbhpMaxRel(), well_state);
            }
            // We have to update the well controls regardless whether there are local
            // wells active or not as parallel logging will take place that needs to
            // communicate with all processes.
            asImpl().wellModel().updateWellControls(well_state);

            if (asImpl().wellModel().wellCollection()->groupControlActive()) {
                // Enforce the VREP control
                applyVREPGroupControl(reservoir_state, well_state);
                asImpl().wellModel().wellCollection()->updateWellTargets(well_state.wellRates());
            }
        } while (it < 15);

        if (converged) {
            if (terminalOutputEnabled())
            {
                OpmLog::note("well converged iter: " + std::to_string(it));
            }

            const int nw = wells().number_of_wells;
            {
                // We will set the bhp primary variable to the new ones,
                // but we do not change the derivatives here.
                ADB::V new_bhp = Eigen::Map<ADB::V>(well_state.bhp().data(), nw);
                // Avoiding the copy below would require a value setter method
                // in AutoDiffBlock.
                std::vector<ADB::M> old_derivs = state.bhp.derivative();
                state.bhp = ADB::function(std::move(new_bhp), std::move(old_derivs));
            }
            {
                // Need to reshuffle well rates, from phase running fastest
                // to wells running fastest.
                // The transpose() below switches the ordering.
                const DataBlock wrates = Eigen::Map<const DataBlock>(well_state.wellRates().data(), nw, np).transpose();
                ADB::V new_qs = Eigen::Map<const V>(wrates.data(), nw*np);
                std::vector<ADB::M> old_derivs = state.qs.derivative();
                state.qs = ADB::function(std::move(new_qs), std::move(old_derivs));
            }
            asImpl().computeWellConnectionPressures(state, well_state);
        }

        if (!converged) {
            well_state = well_state0;
        }

        SimulatorReport report;
        report.total_well_iterations = it;
        report.converged = converged;
        return report;
    }





    template <class Grid, class WellModel, class Implementation>
    V
    BlackoilModelBase<Grid, WellModel, Implementation>::
    solveJacobianSystem() const
    {
        return linsolver_.computeNewtonIncrement(residual_);
    }

    template <class Grid, class WellModel, class Implementation>
    void
    BlackoilModelBase<Grid, WellModel, Implementation>::
    updateState(const V& dx,
                ReservoirState& reservoir_state,
                WellState& well_state)
    {
        using namespace Opm::AutoDiffGrid;
        const int np = fluid_.numPhases();
        const int nc = numCells(grid_);
        const V null;
        assert(null.size() == 0);
        const V zero = V::Zero(nc);
        const V ones = V::Constant(nc,1.0);

        // Extract parts of dx corresponding to each part.
        const V dp = subset(dx, Span(nc));
        int varstart = nc;
        const V dsw = active_[Water] ? subset(dx, Span(nc, 1, varstart)) : null;
        varstart += dsw.size();

        const V dxvar = active_[Gas] ? subset(dx, Span(nc, 1, varstart)): null;
        varstart += dxvar.size();

        // Extract well parts np phase rates + bhp
        const V dwells = subset(dx, Span(asImpl().wellModel().numWellVars(), 1, varstart));
        varstart += dwells.size();

        assert(varstart == dx.size());

        // Pressure update.
        const double dpmaxrel = dpMaxRel();
        const V p_old = Eigen::Map<const V>(&reservoir_state.pressure()[0], nc, 1);
        const V absdpmax = dpmaxrel*p_old.abs();
        const V dp_limited = sign(dp) * dp.abs().min(absdpmax);
        const V p = (p_old - dp_limited).max(zero);
        std::copy(&p[0], &p[0] + nc, reservoir_state.pressure().begin());

        // Saturation updates.
        const Opm::PhaseUsage& pu = fluid_.phaseUsage();
        const DataBlock s_old = Eigen::Map<const DataBlock>(& reservoir_state.saturation()[0], nc, np);
        const double dsmax = dsMax();

        V so;
        V sw;
        V sg;

        {
            V maxVal = zero;
            V dso = zero;
            if (active_[Water]){
                maxVal = dsw.abs().max(maxVal);
                dso = dso - dsw;
            }

            V dsg;
            if (active_[Gas]){
                dsg = isSg_ * dxvar - isRv_ * dsw;
                maxVal = dsg.abs().max(maxVal);
                dso = dso - dsg;
            }

            maxVal = dso.abs().max(maxVal);

            V step = dsmax/maxVal;
            step = step.min(1.);

            if (active_[Water]) {
                const int pos = pu.phase_pos[ Water ];
                const V sw_old = s_old.col(pos);
                sw = sw_old - step * dsw;
            }

            if (active_[Gas]) {
                const int pos = pu.phase_pos[ Gas ];
                const V sg_old = s_old.col(pos);
                sg = sg_old - step * dsg;
            }

            assert(active_[Oil]);
            const int pos = pu.phase_pos[ Oil ];
            const V so_old = s_old.col(pos);
            so = so_old - step * dso;
        }

        if (active_[Gas]) {
            auto ixg = sg < 0;
            for (int c = 0; c < nc; ++c) {
                if (ixg[c]) {
                    if (active_[Water]) {
                        sw[c] = sw[c] / (1-sg[c]);
                    }
                    so[c] = so[c] / (1-sg[c]);
                    sg[c] = 0;
                }
            }
        }

        if (active_[Oil]) {
            auto ixo = so < 0;
            for (int c = 0; c < nc; ++c) {
                if (ixo[c]) {
                    if (active_[Water]) {
                        sw[c] = sw[c] / (1-so[c]);
                    }
                    if (active_[Gas]) {
                        sg[c] = sg[c] / (1-so[c]);
                    }
                    so[c] = 0;
                }
            }
        }

        if (active_[Water]) {
            auto ixw = sw < 0;
            for (int c = 0; c < nc; ++c) {
                if (ixw[c]) {
                    so[c] = so[c] / (1-sw[c]);
                    if (active_[Gas]) {
                        sg[c] = sg[c] / (1-sw[c]);
                    }
                    sw[c] = 0;
                }
            }
        }

        // Update rs and rv
        const double drmaxrel = drMaxRel();
        V rs;
        if (has_disgas_) {
            const V rs_old = Eigen::Map<const V>(&reservoir_state.gasoilratio()[0], nc);
            const V drs = isRs_ * dxvar;
            const V drs_limited = sign(drs) * drs.abs().min( (rs_old.abs()*drmaxrel).max( ones*1e-6));
            rs = rs_old - drs_limited;
            rs = rs.max(zero);
        }
        V rv;
        if (has_vapoil_) {
            const V rv_old = Eigen::Map<const V>(&reservoir_state.rv()[0], nc);
            const V drv = isRv_ * dxvar;
            const V drv_limited = sign(drv) * drv.abs().min( (rv_old.abs()*drmaxrel).max( ones*1e-6));
            rv = rv_old - drv_limited;
            rv = rv.max(zero);
        }

        // Sg is used as primal variable for water only cells.
        const double epsilon = std::sqrt(std::numeric_limits<double>::epsilon());
        auto watOnly = sw >  (1 - epsilon);

        // phase translation sg <-> rs
        std::vector<HydroCarbonState>& hydroCarbonState = reservoir_state.hydroCarbonState();
        std::fill(hydroCarbonState.begin(), hydroCarbonState.end(), HydroCarbonState::GasAndOil);

        if (has_disgas_) {
            const V rsSat0 = fluidRsSat(p_old, s_old.col(pu.phase_pos[Oil]), cells_);
            const V rsSat = fluidRsSat(p, so, cells_);
            sd_.rsSat = ADB::constant(rsSat);
            // The obvious case
            auto hasGas = (sg > 0 && isRs_ == 0);

            // Set oil saturated if previous rs is sufficiently large
            const V rs_old = Eigen::Map<const V>(&reservoir_state.gasoilratio()[0], nc);
            auto gasVaporized =  ( (rs > rsSat * (1+epsilon) && isRs_ == 1 ) && (rs_old > rsSat0 * (1-epsilon)) );
            auto useSg = watOnly || hasGas || gasVaporized;
            for (int c = 0; c < nc; ++c) {
                if (useSg[c]) {
                    rs[c] = rsSat[c];
                    if (watOnly[c]) {
                        so[c] = 0;
                        sg[c] = 0;
                        rs[c] = 0;
                    }
                } else {
                    hydroCarbonState[c] = HydroCarbonState::OilOnly;
                }
            }
            rs = rs.min(rsSat);
        }

        // phase transitions so <-> rv
        if (has_vapoil_) {

            // The gas pressure is needed for the rvSat calculations
            const V gaspress_old = computeGasPressure(p_old, s_old.col(Water), s_old.col(Oil), s_old.col(Gas));
            const V gaspress = computeGasPressure(p, sw, so, sg);
            const V rvSat0 = fluidRvSat(gaspress_old, s_old.col(pu.phase_pos[Oil]), cells_);
            const V rvSat = fluidRvSat(gaspress, so, cells_);
            sd_.rvSat = ADB::constant(rvSat);

            // The obvious case
            auto hasOil = (so > 0 && isRv_ == 0);

            // Set oil saturated if previous rv is sufficiently large
            const V rv_old = Eigen::Map<const V>(&reservoir_state.rv()[0], nc);
            auto oilCondensed = ( (rv > rvSat * (1+epsilon) && isRv_ == 1) && (rv_old > rvSat0 * (1-epsilon)) );
            auto useSg = watOnly || hasOil || oilCondensed;
            for (int c = 0; c < nc; ++c) {
                if (useSg[c]) {
                    rv[c] = rvSat[c];
                    if (watOnly[c]) {
                        so[c] = 0;
                        sg[c] = 0;
                        rv[c] = 0;
                    }
                } else {
                    hydroCarbonState[c] = HydroCarbonState::GasOnly;
                }
            }
            rv = rv.min(rvSat);
        }

        // Update the reservoir_state
        if (active_[Water]) {
            for (int c = 0; c < nc; ++c) {
                reservoir_state.saturation()[c*np + pu.phase_pos[ Water ]] = sw[c];
            }
        }

        if (active_[Gas]) {
            for (int c = 0; c < nc; ++c) {
                reservoir_state.saturation()[c*np + pu.phase_pos[ Gas ]] = sg[c];
            }
        }

        if (active_[ Oil ]) {
            for (int c = 0; c < nc; ++c) {
                reservoir_state.saturation()[c*np + pu.phase_pos[ Oil ]] = so[c];
            }
        }

        if (has_disgas_) {
            std::copy(&rs[0], &rs[0] + nc, reservoir_state.gasoilratio().begin());
        }

        if (has_vapoil_) {
            std::copy(&rv[0], &rv[0] + nc, reservoir_state.rv().begin());
        }

        asImpl().wellModel().updateWellState(dwells, dbhpMaxRel(), well_state);

        // Update phase conditions used for property calculations.
        updatePhaseCondFromPrimalVariable(reservoir_state);
    }





    template <class Grid, class WellModel, class Implementation>
    std::vector<ADB>
    BlackoilModelBase<Grid, WellModel, Implementation>::
    computeRelPerm(const SolutionState& state) const
    {
        using namespace Opm::AutoDiffGrid;
        const int               nc   = numCells(grid_);

        const ADB zero = ADB::constant(V::Zero(nc));

        const Opm::PhaseUsage& pu = fluid_.phaseUsage();
        const ADB& sw = (active_[ Water ]
                         ? state.saturation[ pu.phase_pos[ Water ] ]
                         : zero);

        const ADB& so = (active_[ Oil ]
                         ? state.saturation[ pu.phase_pos[ Oil ] ]
                         : zero);

        const ADB& sg = (active_[ Gas ]
                         ? state.saturation[ pu.phase_pos[ Gas ] ]
                         : zero);

        return fluid_.relperm(sw, so, sg, cells_);
    }





    template <class Grid, class WellModel, class Implementation>
    std::vector<ADB>
    BlackoilModelBase<Grid, WellModel, Implementation>::
    computePressures(const ADB& po,
                     const ADB& sw,
                     const ADB& so,
                     const ADB& sg) const
    {
        // convert the pressure offsets to the capillary pressures
        std::vector<ADB> pressure = fluid_.capPress(sw, so, sg, cells_);
        for (int phaseIdx = 0; phaseIdx < BlackoilPhases::MaxNumPhases; ++phaseIdx) {
            // The reference pressure is always the liquid phase (oil) pressure.
            if (phaseIdx == BlackoilPhases::Liquid)
                continue;
            if (active_[phaseIdx]) {
                pressure[phaseIdx] = pressure[phaseIdx] - pressure[BlackoilPhases::Liquid];
            }
        }

        // Since pcow = po - pw, but pcog = pg - po,
        // we have
        //   pw = po - pcow
        //   pg = po + pcgo
        // This is an unfortunate inconsistency, but a convention we must handle.
        for (int phaseIdx = 0; phaseIdx < BlackoilPhases::MaxNumPhases; ++phaseIdx) {
            if (active_[phaseIdx]) {
                if (phaseIdx == BlackoilPhases::Aqua) {
                    pressure[phaseIdx] = po - pressure[phaseIdx];
                } else {
                    pressure[phaseIdx] += po;
                }
            }
        }

        return pressure;
    }





    template <class Grid, class WellModel, class Implementation>
    V
    BlackoilModelBase<Grid, WellModel, Implementation>::
    computeGasPressure(const V& po,
                       const V& sw,
                       const V& so,
                       const V& sg) const
    {
        assert (active_[Gas]);
        std::vector<ADB> cp = fluid_.capPress(ADB::constant(sw),
                                              ADB::constant(so),
                                              ADB::constant(sg),
                                              cells_);
        return cp[Gas].value() + po;
    }



    template <class Grid, class WellModel, class Implementation>
    void
    BlackoilModelBase<Grid, WellModel, Implementation>::
    computeMassFlux(const int               actph ,
                    const V&                transi,
                    const ADB&              kr    ,
                    const ADB&              mu    ,
                    const ADB&              rho   ,
                    const ADB&              phasePressure,
                    const SolutionState&    state)
    {
        // Compute and store mobilities.
        const ADB tr_mult = transMult(state.pressure);
        sd_.rq[ actph ].mob = tr_mult * kr / mu;

        // Compute head differentials. Gravity potential is done using the face average as in eclipse and MRST.
        const ADB rhoavg = ops_.caver * rho;
        sd_.rq[ actph ].dh = ops_.ngrad * phasePressure - geo_.gravity()[2] * (rhoavg * (ops_.ngrad * geo_.z().matrix()));
        if (use_threshold_pressure_) {
            applyThresholdPressures(sd_.rq[ actph ].dh);
        }

        // Compute phase fluxes with upwinding of formation value factor and mobility.
        const ADB& b   = sd_.rq[ actph ].b;
        const ADB& mob = sd_.rq[ actph ].mob;
        const ADB& dh  = sd_.rq[ actph ].dh;
        UpwindSelector<double> upwind(grid_, ops_, dh.value());
        sd_.rq[ actph ].mflux = upwind.select(b * mob) * (transi * dh);
    }





    template <class Grid, class WellModel, class Implementation>
    void
    BlackoilModelBase<Grid, WellModel, Implementation>::
    applyThresholdPressures(ADB& dp)
    {
        // We support reversible threshold pressures only.
        // Method: if the potential difference is lower (in absolute
        // value) than the threshold for any face, then the potential
        // (and derivatives) is set to zero. If it is above the
        // threshold, the threshold pressure is subtracted from the
        // absolute potential (the potential is moved towards zero).

        // Identify the set of faces where the potential is under the
        // threshold, that shall have zero flow. Storing the bool
        // Array as a V (a double Array) with 1 and 0 elements, a
        // 1 where flow is allowed, a 0 where it is not.
        const V high_potential = (dp.value().abs() >= threshold_pressures_by_connection_).template cast<double>();

        // Create a sparse vector that nullifies the low potential elements.
        const M keep_high_potential(high_potential.matrix().asDiagonal());

        // Find the current sign for the threshold modification
        const V sign_dp = sign(dp.value());
        const V threshold_modification = sign_dp * threshold_pressures_by_connection_;

        // Modify potential and nullify where appropriate.
        dp = keep_high_potential * (dp - threshold_modification);
    }





    template <class Grid, class WellModel, class Implementation>
    std::vector<double>
    BlackoilModelBase<Grid, WellModel, Implementation>::
    computeResidualNorms() const
    {
        std::vector<double> residualNorms;

        std::vector<ADB>::const_iterator massBalanceIt = residual_.material_balance_eq.begin();
        const std::vector<ADB>::const_iterator endMassBalanceIt = residual_.material_balance_eq.end();

        for (; massBalanceIt != endMassBalanceIt; ++massBalanceIt) {
            const double massBalanceResid = detail::infinityNorm( (*massBalanceIt),
                                                                  linsolver_.parallelInformation() );
            if (!std::isfinite(massBalanceResid)) {
                OPM_THROW(Opm::NumericalProblem,
                          "Encountered a non-finite residual");
            }
            residualNorms.push_back(massBalanceResid);
        }

        // the following residuals are not used in the oscillation detection now
        const double wellFluxResid = detail::infinityNormWell( residual_.well_flux_eq,
                                                               linsolver_.parallelInformation() );
        if (!std::isfinite(wellFluxResid)) {
            OPM_THROW(Opm::NumericalProblem,
               "Encountered a non-finite residual");
        }
        residualNorms.push_back(wellFluxResid);

        const double wellResid = detail::infinityNormWell( residual_.well_eq,
                                                           linsolver_.parallelInformation() );
        if (!std::isfinite(wellResid)) {
           OPM_THROW(Opm::NumericalProblem,
               "Encountered a non-finite residual");
        }
        residualNorms.push_back(wellResid);

        return residualNorms;
    }


    template <class Grid, class WellModel, class Implementation>
    double
    BlackoilModelBase<Grid, WellModel, Implementation>::
    relativeChange(const SimulationDataContainer& previous,
                   const SimulationDataContainer& current ) const
    {
        std::vector< double > p0  ( previous.pressure() );
        std::vector< double > sat0( previous.saturation() );

        const std::size_t pSize = p0.size();
        const std::size_t satSize = sat0.size();

        // compute u^n - u^n+1
        for( std::size_t i=0; i<pSize; ++i ) {
            p0[ i ] -= current.pressure()[ i ];
        }

        for( std::size_t i=0; i<satSize; ++i ) {
            sat0[ i ] -= current.saturation()[ i ];
        }

        // compute || u^n - u^n+1 ||
        const double stateOld  = detail::euclidianNormSquared( p0.begin(),   p0.end(), 1, linsolver_.parallelInformation() ) +
                                 detail::euclidianNormSquared( sat0.begin(), sat0.end(),
                                                               current.numPhases(),
                                                               linsolver_.parallelInformation() );

        // compute || u^n+1 ||
        const double stateNew  = detail::euclidianNormSquared( current.pressure().begin(),   current.pressure().end(), 1, linsolver_.parallelInformation() ) +
                                 detail::euclidianNormSquared( current.saturation().begin(), current.saturation().end(),
                                                               current.numPhases(),
                                                               linsolver_.parallelInformation() );

        if( stateNew > 0.0 ) {
            return stateOld / stateNew ;
        }
        else {
            return 0.0;
        }
    }

    template <class Grid, class WellModel, class Implementation>
    double
    BlackoilModelBase<Grid, WellModel, Implementation>::
    convergenceReduction(const Eigen::Array<double, Eigen::Dynamic, Eigen::Dynamic>& B,
                         const Eigen::Array<double, Eigen::Dynamic, Eigen::Dynamic>& tempV,
                         const Eigen::Array<double, Eigen::Dynamic, Eigen::Dynamic>& R,
                         std::vector<double>& R_sum,
                         std::vector<double>& maxCoeff,
                         std::vector<double>& B_avg,
                         std::vector<double>& maxNormWell,
                         int nc) const
    {
        const int np = asImpl().numPhases();
        const int nm = asImpl().numMaterials();
        const int nw = residual_.well_flux_eq.size() / np;
        assert(nw * np == int(residual_.well_flux_eq.size()));

        // Do the global reductions
#if HAVE_MPI
        if ( linsolver_.parallelInformation().type() == typeid(ParallelISTLInformation) )
        {
            const ParallelISTLInformation& info =
                boost::any_cast<const ParallelISTLInformation&>(linsolver_.parallelInformation());

            // Compute the global number of cells and porevolume
            std::vector<int> v(nc, 1);
            auto nc_and_pv = std::tuple<int, double>(0, 0.0);
            auto nc_and_pv_operators = std::make_tuple(Opm::Reduction::makeGlobalSumFunctor<int>(),
                                                        Opm::Reduction::makeGlobalSumFunctor<double>());
            auto nc_and_pv_containers  = std::make_tuple(v, geo_.poreVolume());
            info.computeReduction(nc_and_pv_containers, nc_and_pv_operators, nc_and_pv);

            for ( int idx = 0; idx < nm; ++idx )
            {
                auto values     = std::tuple<double,double,double>(0.0 ,0.0 ,0.0);
                auto containers = std::make_tuple(B.col(idx),
                                                  tempV.col(idx),
                                                  R.col(idx));
                auto operators  = std::make_tuple(Opm::Reduction::makeGlobalSumFunctor<double>(),
                                                  Opm::Reduction::makeGlobalMaxFunctor<double>(),
                                                  Opm::Reduction::makeGlobalSumFunctor<double>());
                info.computeReduction(containers, operators, values);
                B_avg[idx]       = std::get<0>(values)/std::get<0>(nc_and_pv);
                maxCoeff[idx]    = std::get<1>(values);
                R_sum[idx]       = std::get<2>(values);
                assert(nm >= np);
                if (idx < np) {
                    maxNormWell[idx] = 0.0;
                    for ( int w = 0; w < nw; ++w ) {
                        maxNormWell[idx]  = std::max(maxNormWell[idx], std::abs(residual_.well_flux_eq.value()[nw*idx + w]));
                    }
                }
            }
            info.communicator().max(maxNormWell.data(), np);
            // Compute pore volume
            return std::get<1>(nc_and_pv);
        }
        else
#endif
        {
            B_avg.resize(nm);
            maxCoeff.resize(nm);
            R_sum.resize(nm);
            maxNormWell.resize(np);
            for ( int idx = 0; idx < nm; ++idx )
            {
                B_avg[idx] = B.col(idx).sum()/nc;
                maxCoeff[idx] = tempV.col(idx).maxCoeff();
                R_sum[idx] = R.col(idx).sum();

                assert(nm >= np);
                if (idx < np) {
                    maxNormWell[idx] = 0.0;
                    for ( int w = 0; w < nw; ++w ) {
                        maxNormWell[idx] = std::max(maxNormWell[idx], std::abs(residual_.well_flux_eq.value()[nw*idx + w]));
                    }
                }
            }
            // Compute total pore volume
            return geo_.poreVolume().sum();
        }
    }





    template <class Grid, class WellModel, class Implementation>
    bool
    BlackoilModelBase<Grid, WellModel, Implementation>::
    getConvergence(const SimulatorTimerInterface& timer, const int iteration)
    {
        const double dt = timer.currentStepLength();
        const double tol_mb    = param_.tolerance_mb_;
        const double tol_cnv   = param_.tolerance_cnv_;
        const double tol_wells = param_.tolerance_wells_;
        const double tol_well_control = param_.tolerance_well_control_;

        const int nc = Opm::AutoDiffGrid::numCells(grid_);
        const int np = asImpl().numPhases();
        const int nm = asImpl().numMaterials();
        assert(int(sd_.rq.size()) == nm);

        const V& pv = geo_.poreVolume();

        std::vector<double> R_sum(nm);
        std::vector<double> B_avg(nm);
        std::vector<double> maxCoeff(nm);
        std::vector<double> maxNormWell(np);
        Eigen::Array<V::Scalar, Eigen::Dynamic, Eigen::Dynamic> B(nc, nm);
        Eigen::Array<V::Scalar, Eigen::Dynamic, Eigen::Dynamic> R(nc, nm);
        Eigen::Array<V::Scalar, Eigen::Dynamic, Eigen::Dynamic> tempV(nc, nm);

        for ( int idx = 0; idx < nm; ++idx )
        {
            const ADB& tempB = sd_.rq[idx].b;
            B.col(idx)       = 1./tempB.value();
            R.col(idx)       = residual_.material_balance_eq[idx].value();
            tempV.col(idx)   = R.col(idx).abs()/pv;
        }

        const double pvSum = convergenceReduction(B, tempV, R,
                                                  R_sum, maxCoeff, B_avg, maxNormWell,
                                                  nc);

        std::vector<double> CNV(nm);
        std::vector<double> mass_balance_residual(nm);
        std::vector<double> well_flux_residual(np);

        bool converged_MB = true;
        bool converged_CNV = true;
        bool converged_Well = true;
        // Finish computation
        for ( int idx = 0; idx < nm; ++idx )
        {
            CNV[idx]                    = B_avg[idx] * dt * maxCoeff[idx];
            mass_balance_residual[idx]  = std::abs(B_avg[idx]*R_sum[idx]) * dt / pvSum;
            converged_MB                = converged_MB && (mass_balance_residual[idx] < tol_mb);
            converged_CNV               = converged_CNV && (CNV[idx] < tol_cnv);
            // Well flux convergence is only for fluid phases, not other materials
            // in our current implementation.
            assert(nm >= np);
            if (idx < np) {
                well_flux_residual[idx] = B_avg[idx] * maxNormWell[idx];
                converged_Well = converged_Well && (well_flux_residual[idx] < tol_wells);
            }
        }

        const double residualWell     = detail::infinityNormWell(residual_.well_eq,
                                                                 linsolver_.parallelInformation());
        converged_Well = converged_Well && (residualWell < tol_well_control);

        const bool converged = converged_MB && converged_CNV && converged_Well;

        // Residual in Pascal can have high values and still be ok.
        const double maxWellResidualAllowed = 1000.0 * maxResidualAllowed();

        if ( terminal_output_ )
        {
            // Only rank 0 does print to std::cout
            if (iteration == 0) {
                std::string msg = "Iter";
                for (int idx = 0; idx < nm; ++idx) {
                    msg += "   MB(" + materialName(idx).substr(0, 3) + ") ";
                }
                for (int idx = 0; idx < nm; ++idx) {
                    msg += "    CNV(" + materialName(idx).substr(0, 1) + ") ";
                }
                for (int idx = 0; idx < np; ++idx) {
                    msg += "  W-FLUX(" + materialName(idx).substr(0, 1) + ")";
                }
                msg += "  WELL-CONT";
                // std::cout << "  WELL-CONT ";
                OpmLog::note(msg);
            }
            std::ostringstream ss;
            const std::streamsize oprec = ss.precision(3);
            const std::ios::fmtflags oflags = ss.setf(std::ios::scientific);
            ss << std::setw(4) << iteration;
            for (int idx = 0; idx < nm; ++idx) {
                ss << std::setw(11) << mass_balance_residual[idx];
            }
            for (int idx = 0; idx < nm; ++idx) {
                ss << std::setw(11) << CNV[idx];
            }
            for (int idx = 0; idx < np; ++idx) {
                ss << std::setw(11) << well_flux_residual[idx];
            }
            ss << std::setw(11) << residualWell;
            // std::cout << std::setw(11) << residualWell;
            ss.precision(oprec);
            ss.flags(oflags);
            OpmLog::note(ss.str());
        }

        for (int idx = 0; idx < nm; ++idx) {
            if (std::isnan(mass_balance_residual[idx])
                || std::isnan(CNV[idx])
                || (idx < np && std::isnan(well_flux_residual[idx]))) {
                const auto msg = std::string("NaN residual for phase ") +  materialName(idx);
                if (terminal_output_) {
                    OpmLog::problem(msg);
                }
                OPM_THROW_NOLOG(Opm::NumericalProblem, msg);
            }
            if (mass_balance_residual[idx] > maxResidualAllowed()
                || CNV[idx] > maxResidualAllowed()
                || (idx < np && well_flux_residual[idx] > maxResidualAllowed())) {
                const auto msg = std::string("Too large residual for phase ") +  materialName(idx);
                if (terminal_output_) {
                    OpmLog::problem(msg);
                }
                OPM_THROW_NOLOG(Opm::NumericalProblem, msg);
            }
        }
        if (std::isnan(residualWell) || residualWell > maxWellResidualAllowed) {
            const auto msg = std::string("NaN or too large residual for well control equation");
            if (terminal_output_) {
                OpmLog::problem(msg);
            }
            OPM_THROW_NOLOG(Opm::NumericalProblem, msg);
        }

        return converged;
    }





    template <class Grid, class WellModel, class Implementation>
    bool
    BlackoilModelBase<Grid, WellModel, Implementation>::
    getWellConvergence(const int iteration)
    {
        const double tol_wells = param_.tolerance_wells_;
        const double tol_well_control = param_.tolerance_well_control_;

        const int nc = Opm::AutoDiffGrid::numCells(grid_);
        const int np = asImpl().numPhases();
        const int nm = asImpl().numMaterials();

        const V& pv = geo_.poreVolume();
        std::vector<double> R_sum(nm);
        std::vector<double> B_avg(nm);
        std::vector<double> maxCoeff(nm);
        std::vector<double> maxNormWell(np);
        Eigen::Array<V::Scalar, Eigen::Dynamic, Eigen::Dynamic> B(nc, nm);
        Eigen::Array<V::Scalar, Eigen::Dynamic, Eigen::Dynamic> R(nc, nm);
        Eigen::Array<V::Scalar, Eigen::Dynamic, Eigen::Dynamic> tempV(nc, nm);
        for ( int idx = 0; idx < nm; ++idx )
        {
            const ADB& tempB = sd_.rq[idx].b;
            B.col(idx)       = 1./tempB.value();
            R.col(idx)       = residual_.material_balance_eq[idx].value();
            tempV.col(idx)   = R.col(idx).abs()/pv;
        }

        convergenceReduction(B, tempV, R, R_sum, maxCoeff, B_avg, maxNormWell, nc);

        std::vector<double> well_flux_residual(np);
        bool converged_Well = true;
        // Finish computation
        for ( int idx = 0; idx < np; ++idx )
        {
            well_flux_residual[idx] = B_avg[idx] * maxNormWell[idx];
            converged_Well = converged_Well && (well_flux_residual[idx] < tol_wells);
        }

        const double residualWell     = detail::infinityNormWell(residual_.well_eq,
                                                                 linsolver_.parallelInformation());
        converged_Well = converged_Well && (residualWell < tol_well_control);

        const bool converged = converged_Well;

        // if one of the residuals is NaN, throw exception, so that the solver can be restarted
        for (int idx = 0; idx < np; ++idx) {
            if (std::isnan(well_flux_residual[idx])) {
                const auto msg = std::string("NaN residual for phase ") +  materialName(idx);
                if (terminal_output_) {
                    OpmLog::problem(msg);
                }
                OPM_THROW_NOLOG(Opm::NumericalProblem, msg);
            }
            if (well_flux_residual[idx] > maxResidualAllowed()) {
                const auto msg = std::string("Too large residual for phase ") +  materialName(idx);
                if (terminal_output_) {
                    OpmLog::problem(msg);
                }
                OPM_THROW_NOLOG(Opm::NumericalProblem, msg);
            }
        }

        if ( terminal_output_ )
        {
            // Only rank 0 does print to std::cout
            if (iteration == 0) {
                std::string msg;
                msg = "Iter";
                for (int idx = 0; idx < np; ++idx) {
                    msg += "  W-FLUX(" + materialName(idx).substr(0, 1) + ")";
                }
                msg += "  WELL-CONT";
                OpmLog::note(msg);
            }
            std::ostringstream ss;
            const std::streamsize oprec = ss.precision(3);
            const std::ios::fmtflags oflags = ss.setf(std::ios::scientific);
            ss << std::setw(4) << iteration;
            for (int idx = 0; idx < np; ++idx) {
                ss << std::setw(11) << well_flux_residual[idx];
            }
            ss << std::setw(11) << residualWell;
            ss.precision(oprec);
            ss.flags(oflags);
            OpmLog::note(ss.str());
        }
        return converged;
    }





    template <class Grid, class WellModel, class Implementation>
    ADB
    BlackoilModelBase<Grid, WellModel, Implementation>::
    fluidViscosity(const int               phase,
                   const ADB&              p    ,
                   const ADB&              temp ,
                   const ADB&              rs   ,
                   const ADB&              rv   ,
                   const std::vector<PhasePresence>& cond) const
    {
        switch (phase) {
        case Water:
            return fluid_.muWat(p, temp, cells_);
        case Oil:
            return fluid_.muOil(p, temp, rs, cond, cells_);
        case Gas:
            return fluid_.muGas(p, temp, rv, cond, cells_);
        default:
            OPM_THROW(std::runtime_error, "Unknown phase index " << phase);
        }
    }





    template <class Grid, class WellModel, class Implementation>
    ADB
    BlackoilModelBase<Grid, WellModel, Implementation>::
    fluidReciprocFVF(const int               phase,
                     const ADB&              p    ,
                     const ADB&              temp ,
                     const ADB&              rs   ,
                     const ADB&              rv   ,
                     const std::vector<PhasePresence>& cond) const
    {
        switch (phase) {
        case Water:
            return fluid_.bWat(p, temp, cells_);
        case Oil:
            return fluid_.bOil(p, temp, rs, cond, cells_);
        case Gas:
            return fluid_.bGas(p, temp, rv, cond, cells_);
        default:
            OPM_THROW(std::runtime_error, "Unknown phase index " << phase);
        }
    }





    template <class Grid, class WellModel, class Implementation>
    ADB
    BlackoilModelBase<Grid, WellModel, Implementation>::
    fluidDensity(const int  phase,
                 const ADB& b,
                 const ADB& rs,
                 const ADB& rv) const
    {
        const V& rhos = fluid_.surfaceDensity(phase,  cells_);
        const Opm::PhaseUsage& pu = fluid_.phaseUsage();
        ADB rho = rhos * b;
        if (phase == Oil && active_[Gas]) {
            rho += fluid_.surfaceDensity(pu.phase_pos[ Gas ],  cells_) * rs * b;
        }
        if (phase == Gas && active_[Oil]) {
            rho += fluid_.surfaceDensity(pu.phase_pos[ Oil ],  cells_) * rv * b;
        }
        return rho;
    }





    template <class Grid, class WellModel, class Implementation>
    V
    BlackoilModelBase<Grid, WellModel, Implementation>::
    fluidRsSat(const V&                p,
               const V&                satOil,
               const std::vector<int>& cells) const
    {
        return fluid_.rsSat(ADB::constant(p), ADB::constant(satOil), cells).value();
    }





    template <class Grid, class WellModel, class Implementation>
    ADB
    BlackoilModelBase<Grid, WellModel, Implementation>::
    fluidRsSat(const ADB&              p,
               const ADB&              satOil,
               const std::vector<int>& cells) const
    {
        return fluid_.rsSat(p, satOil, cells);
    }





    template <class Grid, class WellModel, class Implementation>
    V
    BlackoilModelBase<Grid, WellModel, Implementation>::
    fluidRvSat(const V&                p,
               const V&              satOil,
               const std::vector<int>& cells) const
    {
        return fluid_.rvSat(ADB::constant(p), ADB::constant(satOil), cells).value();
    }





    template <class Grid, class WellModel, class Implementation>
    ADB
    BlackoilModelBase<Grid, WellModel, Implementation>::
    fluidRvSat(const ADB&              p,
               const ADB&              satOil,
               const std::vector<int>& cells) const
    {
        return fluid_.rvSat(p, satOil, cells);
    }





    template <class Grid, class WellModel, class Implementation>
    ADB
    BlackoilModelBase<Grid, WellModel, Implementation>::
    poroMult(const ADB& p) const
    {
        const int n = p.size();
        if (rock_comp_props_ && rock_comp_props_->isActive()) {
            V pm(n);
            V dpm(n);
#pragma omp parallel for schedule(static)
            for (int i = 0; i < n; ++i) {
                pm[i] = rock_comp_props_->poroMult(p.value()[i]);
                dpm[i] = rock_comp_props_->poroMultDeriv(p.value()[i]);
            }
            ADB::M dpm_diag(dpm.matrix().asDiagonal());
            const int num_blocks = p.numBlocks();
            std::vector<ADB::M> jacs(num_blocks);
#pragma omp parallel for schedule(dynamic)
            for (int block = 0; block < num_blocks; ++block) {
                fastSparseProduct(dpm_diag, p.derivative()[block], jacs[block]);
            }
            return ADB::function(std::move(pm), std::move(jacs));
        } else {
            return ADB::constant(V::Constant(n, 1.0));
        }
    }





    template <class Grid, class WellModel, class Implementation>
    ADB
    BlackoilModelBase<Grid, WellModel, Implementation>::
    transMult(const ADB& p) const
    {
        const int n = p.size();
        if (rock_comp_props_ && rock_comp_props_->isActive()) {
            V tm(n);
            V dtm(n);
#pragma omp parallel for schedule(static)
            for (int i = 0; i < n; ++i) {
                tm[i] = rock_comp_props_->transMult(p.value()[i]);
                dtm[i] = rock_comp_props_->transMultDeriv(p.value()[i]);
            }
            ADB::M dtm_diag(dtm.matrix().asDiagonal());
            const int num_blocks = p.numBlocks();
            std::vector<ADB::M> jacs(num_blocks);
#pragma omp parallel for schedule(dynamic)
            for (int block = 0; block < num_blocks; ++block) {
                fastSparseProduct(dtm_diag, p.derivative()[block], jacs[block]);
            }
            return ADB::function(std::move(tm), std::move(jacs));
        } else {
            return ADB::constant(V::Constant(n, 1.0));
        }
    }





    template <class Grid, class WellModel, class Implementation>
    void
    BlackoilModelBase<Grid, WellModel, Implementation>::
    classifyCondition(const ReservoirState& state)
    {
        using namespace Opm::AutoDiffGrid;
        const int nc = numCells(grid_);
        const int np = state.numPhases();

        const PhaseUsage& pu = fluid_.phaseUsage();
        const DataBlock s = Eigen::Map<const DataBlock>(& state.saturation()[0], nc, np);
        if (active_[ Gas ]) {
            // Oil/Gas or Water/Oil/Gas system
            const V so = s.col(pu.phase_pos[ Oil ]);
            const V sg = s.col(pu.phase_pos[ Gas ]);

            for (V::Index c = 0, e = sg.size(); c != e; ++c) {
                if (so[c] > 0)        { phaseCondition_[c].setFreeOil  (); }
                if (sg[c] > 0)        { phaseCondition_[c].setFreeGas  (); }
                if (active_[ Water ]) { phaseCondition_[c].setFreeWater(); }
            }
        }
        else {
            // Water/Oil system
            assert (active_[ Water ]);

            const V so = s.col(pu.phase_pos[ Oil ]);


            for (V::Index c = 0, e = so.size(); c != e; ++c) {
                phaseCondition_[c].setFreeWater();

                if (so[c] > 0) { phaseCondition_[c].setFreeOil(); }
            }
        }
    }





    template <class Grid, class WellModel, class Implementation>
    void
    BlackoilModelBase<Grid, WellModel, Implementation>::
    updatePrimalVariableFromState(const ReservoirState& state)
    {
        updatePhaseCondFromPrimalVariable(state);
    }





    /// Update the phaseCondition_ member based on the primalVariable_ member.
    template <class Grid, class WellModel, class Implementation>
    void
    BlackoilModelBase<Grid, WellModel, Implementation>::
    updatePhaseCondFromPrimalVariable(const ReservoirState& state)
    {
        const int nc = Opm::AutoDiffGrid::numCells(grid_);
        isRs_ = V::Zero(nc);
        isRv_ = V::Zero(nc);
        isSg_ = V::Zero(nc);

        if (! (active_[Gas] && active_[Oil])) {
            // updatePhaseCondFromPrimarVariable() logic requires active gas and oil phase.
            phaseCondition_.assign(nc, PhasePresence());
            return;
        }
        for (int c = 0; c < nc; ++c) {
            phaseCondition_[c] = PhasePresence(); // No free phases.
            phaseCondition_[c].setFreeWater(); // Not necessary for property calculation usage.
            switch (state.hydroCarbonState()[c]) {
            case HydroCarbonState::GasAndOil:
                phaseCondition_[c].setFreeOil();
                phaseCondition_[c].setFreeGas();
                isSg_[c] = 1;
                break;
            case HydroCarbonState::OilOnly:
                phaseCondition_[c].setFreeOil();
                isRs_[c] = 1;
                break;
            case HydroCarbonState::GasOnly:
                phaseCondition_[c].setFreeGas();
                isRv_[c] = 1;
                break;
            default:
                OPM_THROW(std::logic_error, "Unknown primary variable enum value in cell " << c << ": " << state.hydroCarbonState()[c]);
            }
        }
    }





   template <class Grid, class WellModel, class Implementation>
   void
   BlackoilModelBase<Grid, WellModel, Implementation>::
   computeWellConnectionPressures(const SolutionState& state,
                                  const WellState& well_state)
   {
            asImpl().wellModel().computeWellConnectionPressures(state, well_state);
   }





    template <class Grid, class WellModel, class Implementation>
    std::vector<V>
    BlackoilModelBase<Grid, WellModel, Implementation>::
    computeFluidInPlace(const ReservoirState& x,
                        const std::vector<int>& fipnum)
    {
        using namespace Opm::AutoDiffGrid;
        const int nc = numCells(grid_);
        std::vector<ADB> saturation(3, ADB::null());
        const DataBlock s = Eigen::Map<const DataBlock>(& x.saturation()[0], nc, x.numPhases());
        const ADB pressure    = ADB::constant(Eigen::Map<const V>(& x.pressure()[0], nc, 1));
        const ADB temperature = ADB::constant(Eigen::Map<const V>(& x.temperature()[0], nc, 1));
        saturation[Water] = active_[Water] ? ADB::constant(s.col(Water)) : ADB::null();
        saturation[Oil] = active_[Oil] ? ADB::constant(s.col(Oil)) : ADB::constant(V::Zero(nc));
        saturation[Gas] = active_[Gas] ? ADB::constant(s.col(Gas)) : ADB::constant(V::Zero(nc));
        const ADB rs =  ADB::constant(Eigen::Map<const V>(& x.gasoilratio()[0], nc, 1));
        const ADB rv = ADB::constant(Eigen::Map<const V>(& x.rv()[0], nc, 1));
        const auto canonical_phase_pressures = computePressures(pressure, saturation[Water], saturation[Oil], saturation[Gas]);
        const Opm::PhaseUsage& pu = fluid_.phaseUsage();
        const std::vector<PhasePresence> cond = phaseCondition();

        const ADB pv_mult = poroMult(pressure);
        const V& pv = geo_.poreVolume();
        const int maxnp = Opm::BlackoilPhases::MaxNumPhases;
        for (int phase = 0; phase < maxnp; ++phase) {
            if (active_[ phase ]) {
                const int pos = pu.phase_pos[ phase ];
                const auto& b = asImpl().fluidReciprocFVF(phase, canonical_phase_pressures[phase], temperature, rs, rv, cond);
                sd_.fip[phase] = ((pv_mult * b * saturation[pos] * pv).value());
            }
        }

        if (active_[ Oil ] && active_[ Gas ]) {
            // Account for gas dissolved in oil and vaporized oil
            sd_.fip[SimulatorData::FIP_DISSOLVED_GAS] = rs.value() * sd_.fip[SimulatorData::FIP_LIQUID];
            sd_.fip[SimulatorData::FIP_VAPORIZED_OIL] = rv.value() * sd_.fip[SimulatorData::FIP_VAPOUR];
        }

        // For a parallel run this is just a local maximum and needs to be updated later
        int dims = *std::max_element(fipnum.begin(), fipnum.end());
        std::vector<V> values(dims, V::Zero(7));

        const V hydrocarbon = saturation[Oil].value() + saturation[Gas].value();
        V hcpv;
        V pres;

        if ( !isParallel() )
        {
            //Accumulate phases for each region
            for (int phase = 0; phase < maxnp; ++phase) {
                if (active_[ phase ]) {
                    for (int c = 0; c < nc; ++c) {
                        const int region = fipnum[c] - 1;
                        if (region != -1) {
                            values[region][phase] += sd_.fip[phase][c];
                        }
                    }
                }
            }

            //Accumulate RS and RV-volumes for each region
            if (active_[ Oil ] && active_[ Gas ]) {
                for (int c = 0; c < nc; ++c) {
                    const int region = fipnum[c] - 1;
                    if (region != -1) {
                        values[region][SimulatorData::FIP_DISSOLVED_GAS] += sd_.fip[SimulatorData::FIP_DISSOLVED_GAS][c];
                        values[region][SimulatorData::FIP_VAPORIZED_OIL] += sd_.fip[SimulatorData::FIP_VAPORIZED_OIL][c];
                    }
                }
            }


            hcpv = V::Zero(dims);
            pres = V::Zero(dims);

            for (int c = 0; c < nc; ++c) {
                const int region = fipnum[c] - 1;
                if (region != -1) {
                    hcpv[region] += pv[c] * hydrocarbon[c];
                    pres[region] += pv[c] * pressure.value()[c];
                }
            }

            sd_.fip[SimulatorData::FIP_PV] = V::Zero(nc);
            sd_.fip[SimulatorData::FIP_WEIGHTED_PRESSURE] = V::Zero(nc);

            for (int c = 0; c < nc; ++c) {
                const int region = fipnum[c] - 1;
                if (region != -1) {
                    sd_.fip[SimulatorData::FIP_PV][c] = pv[c];

                    //Compute hydrocarbon pore volume weighted average pressure.
                    //If we have no hydrocarbon in region, use pore volume weighted average pressure instead
                    if (hcpv[region] != 0) {
                        sd_.fip[SimulatorData::FIP_WEIGHTED_PRESSURE][c] = pv[c] * pressure.value()[c] * hydrocarbon[c] / hcpv[region];
                    } else {
                        sd_.fip[SimulatorData::FIP_WEIGHTED_PRESSURE][c] = pres[region] / pv[c];
                    }

                    values[region][SimulatorData::FIP_PV] += sd_.fip[SimulatorData::FIP_PV][c];
                    values[region][SimulatorData::FIP_WEIGHTED_PRESSURE] += sd_.fip[SimulatorData::FIP_WEIGHTED_PRESSURE][c];
                }
            }
        }
        else
        {
#if HAVE_MPI
            // mask[c] is 1 if we need to compute something in parallel
            const auto & pinfo =
                boost::any_cast<const ParallelISTLInformation&>(linsolver_.parallelInformation());
            const auto& mask = pinfo.getOwnerMask();
            auto comm = pinfo.communicator();
            // Compute the global dims value and resize values accordingly.
            dims = comm.max(dims);
            values.resize(dims, V::Zero(7));

            //Accumulate phases for each region
            for (int phase = 0; phase < maxnp; ++phase) {
                for (int c = 0; c < nc; ++c) {
                    const int region = fipnum[c] - 1;
                    if (region != -1 && mask[c]) {
                        values[region][phase] += sd_.fip[phase][c];
                    }
                }
            }

            //Accumulate RS and RV-volumes for each region
            if (active_[ Oil ] && active_[ Gas ]) {
                for (int c = 0; c < nc; ++c) {
                    const int region = fipnum[c] - 1;
                    if (region != -1 && mask[c]) {
                        values[region][SimulatorData::FIP_DISSOLVED_GAS] += sd_.fip[SimulatorData::FIP_DISSOLVED_GAS][c];
                        values[region][SimulatorData::FIP_VAPORIZED_OIL] += sd_.fip[SimulatorData::FIP_VAPORIZED_OIL][c];
                    }
                }
            }

            hcpv = V::Zero(dims);
            pres = V::Zero(dims);

            for (int c = 0; c < nc; ++c) {
                const int region = fipnum[c] - 1;
                if (region != -1 && mask[c]) {
                    hcpv[region] += pv[c] * hydrocarbon[c];
                    pres[region] += pv[c] * pressure.value()[c];
                }
            }

            comm.sum(hcpv.data(), hcpv.size());
            comm.sum(pres.data(), pres.size());

            sd_.fip[SimulatorData::FIP_PV] = V::Zero(nc);
            sd_.fip[SimulatorData::FIP_WEIGHTED_PRESSURE] = V::Zero(nc);

            for (int c = 0; c < nc; ++c) {
                const int region = fipnum[c] - 1;
                if (region != -1 && mask[c]) {
                    sd_.fip[SimulatorData::FIP_PV][c] = pv[c];

                    if (hcpv[region] != 0) {
                        sd_.fip[SimulatorData::FIP_WEIGHTED_PRESSURE][c] = pv[c] * pressure.value()[c] * hydrocarbon[c] / hcpv[region];
                    } else {
                        sd_.fip[SimulatorData::FIP_WEIGHTED_PRESSURE][c] = pres[region] / pv[c];
                    }

                    values[region][SimulatorData::FIP_PV] += sd_.fip[SimulatorData::FIP_PV][c];
                    values[region][SimulatorData::FIP_WEIGHTED_PRESSURE] += sd_.fip[SimulatorData::FIP_WEIGHTED_PRESSURE][c];
                }
            }

            // For the frankenstein branch we hopefully can turn values into a vanilla
            // std::vector<double>, use some index magic above, use one communication
            // to sum up the vector entries instead of looping over the regions.
            for(int reg=0; reg < dims; ++reg)
            {
                comm.sum(values[reg].data(), values[reg].size());
            }
#else
            // This should never happen!
            OPM_THROW(std::logic_error, "HAVE_MPI should be defined if we are running in parallel");
#endif
        }

        return values;
    }





    template <class Grid, class WellModel, class Implementation>
    void
    BlackoilModelBase<Grid, WellModel, Implementation>::
    computeWellVoidageRates(const ReservoirState& reservoir_state,
                            const WellState& well_state,
                            std::vector<double>& well_voidage_rates,
                            std::vector<double>& voidage_conversion_coeffs)
    {
        // TODO: for now, we store the voidage rates for all the production wells.
        // For injection wells, the rates are stored as zero.
        // We only store the conversion coefficients for all the injection wells.
        // Later, more delicate model will be implemented here.
        // And for the moment, group control can only work for serial running.
        const int nw = well_state.numWells();
        const int np = numPhases();

        const Wells* wells = asImpl().wellModel().wellsPointer();

        // we calculate the voidage rate for each well, that means the sum of all the phases.
        well_voidage_rates.resize(nw, 0);
        // store the conversion coefficients, while only for the use of injection wells.
        voidage_conversion_coeffs.resize(nw * np, 1.0);

        int global_number_wells = nw;

#if HAVE_MPI
        if ( linsolver_.parallelInformation().type() == typeid(ParallelISTLInformation) )
        {
            const auto& info =
                boost::any_cast<const ParallelISTLInformation&>(linsolver_.parallelInformation());
            global_number_wells = info.communicator().sum(global_number_wells);
            if ( global_number_wells )
            {
                rate_converter_.defineState(reservoir_state, boost::any_cast<const ParallelISTLInformation&>(linsolver_.parallelInformation()));
            }
        }
        else
#endif
        {
            if ( global_number_wells )
            {
                rate_converter_.defineState(reservoir_state);
            }
        }

        std::vector<double> well_rates(np, 0.0);
        std::vector<double> convert_coeff(np, 1.0);


        if ( !well_voidage_rates.empty() ) {
            for (int w = 0; w < nw; ++w) {
                const bool is_producer = wells->type[w] == PRODUCER;

                // not sure necessary to change all the value to be positive
                if (is_producer) {
                    std::transform(well_state.wellRates().begin() + np * w,
                                   well_state.wellRates().begin() + np * (w + 1),
                                   well_rates.begin(), std::negate<double>());

                    // the average hydrocarbon conditions of the whole field will be used
                    const int fipreg = 0; // Not considering FIP for the moment.

                    rate_converter_.calcCoeff(well_rates, fipreg, convert_coeff);
                    well_voidage_rates[w] = std::inner_product(well_rates.begin(), well_rates.end(),
                                                               convert_coeff.begin(), 0.0);
                } else {
                    // TODO: Not sure whether will encounter situation with all zero rates
                    // and whether it will cause problem here.
                    std::copy(well_state.wellRates().begin() + np * w,
                              well_state.wellRates().begin() + np * (w + 1),
                              well_rates.begin());
                    // the average hydrocarbon conditions of the whole field will be used
                    const int fipreg = 0; // Not considering FIP for the moment.
                    rate_converter_.calcCoeff(well_rates, fipreg, convert_coeff);
                    std::copy(convert_coeff.begin(), convert_coeff.end(),
                              voidage_conversion_coeffs.begin() + np * w);
                }
            }
        }
    }





    template <class Grid, class WellModel, class Implementation>
    void
    BlackoilModelBase<Grid, WellModel, Implementation>::
    applyVREPGroupControl(const ReservoirState& reservoir_state,
                          WellState& well_state)
    {
        if (asImpl().wellModel().wellCollection()->havingVREPGroups()) {
            std::vector<double> well_voidage_rates;
            std::vector<double> voidage_conversion_coeffs;
            computeWellVoidageRates(reservoir_state, well_state, well_voidage_rates, voidage_conversion_coeffs);
            asImpl().wellModel().wellCollection()->applyVREPGroupControls(well_voidage_rates, voidage_conversion_coeffs);

            // for the wells under group control, update the currentControls for the well_state
            for (const WellNode* well_node : asImpl().wellModel().wellCollection()->getLeafNodes()) {
                if (well_node->isInjector() && !well_node->individualControl()) {
                    const int well_index = well_node->selfIndex();
                    well_state.currentControls()[well_index] = well_node->groupControlIndex();
                }
            }
        }
    }

} // namespace Opm

#endif // OPM_BLACKOILMODELBASE_IMPL_HEADER_INCLUDED
