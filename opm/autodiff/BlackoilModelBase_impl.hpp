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

#include <opm/autodiff/AutoDiffBlock.hpp>
#include <opm/autodiff/AutoDiffHelpers.hpp>
#include <opm/autodiff/GridHelpers.hpp>
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
#include <opm/core/utility/Units.hpp>
#include <opm/core/well_controls.h>
#include <opm/core/utility/parameters/ParameterGroup.hpp>
#include <opm/parser/eclipse/EclipseState/EclipseState.hpp>

#include <cassert>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <limits>
#include <vector>
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


namespace detail {


    inline
    std::vector<int>
    buildAllCells(const int nc)
    {
        std::vector<int> all_cells(nc);

        for (int c = 0; c < nc; ++c) { all_cells[c] = c; }

        return all_cells;
    }



    template <class PU>
    std::vector<bool>
    activePhases(const PU& pu)
    {
        const int maxnp = Opm::BlackoilPhases::MaxNumPhases;
        std::vector<bool> active(maxnp, false);

        for (int p = 0; p < pu.MaxNumPhases; ++p) {
            active[ p ] = pu.phase_used[ p ] != 0;
        }

        return active;
    }



    template <class PU>
    std::vector<int>
    active2Canonical(const PU& pu)
    {
        const int maxnp = Opm::BlackoilPhases::MaxNumPhases;
        std::vector<int> act2can(maxnp, -1);

        for (int phase = 0; phase < maxnp; ++phase) {
            if (pu.phase_used[ phase ]) {
                act2can[ pu.phase_pos[ phase ] ] = phase;
            }
        }

        return act2can;
    }

} // namespace detail


    template <class Grid, class Implementation>
    BlackoilModelBase<Grid, Implementation>::
    BlackoilModelBase(const ModelParameters&          param,
                  const Grid&                     grid ,
                  const BlackoilPropsAdInterface& fluid,
                  const DerivedGeology&           geo  ,
                  const RockCompressibility*      rock_comp_props,
                  const Wells*                    wells_arg,
                  const NewtonIterationBlackoilInterface&    linsolver,
                  Opm::EclipseStateConstPtr eclState,
                  const bool has_disgas,
                  const bool has_vapoil,
                  const bool terminal_output)
        : grid_  (grid)
        , fluid_ (fluid)
        , geo_   (geo)
        , rock_comp_props_(rock_comp_props)
        , wells_ (wells_arg)
        , vfp_properties_(eclState->getTableManager()->getVFPInjTables(), eclState->getTableManager()->getVFPProdTables())
        , linsolver_ (linsolver)
        , active_(detail::activePhases(fluid.phaseUsage()))
        , canph_ (detail::active2Canonical(fluid.phaseUsage()))
        , cells_ (detail::buildAllCells(Opm::AutoDiffGrid::numCells(grid)))
        , ops_   (grid, eclState)
        , wops_  (wells_)
        , has_disgas_(has_disgas)
        , has_vapoil_(has_vapoil)
        , param_( param )
        , use_threshold_pressure_(false)
        , rq_    (fluid.numPhases())
        , phaseCondition_(AutoDiffGrid::numCells(grid))
        , isRs_(V::Zero(AutoDiffGrid::numCells(grid)))
        , isRv_(V::Zero(AutoDiffGrid::numCells(grid)))
        , isSg_(V::Zero(AutoDiffGrid::numCells(grid)))
        , residual_ ( { std::vector<ADB>(fluid.numPhases(), ADB::null()),
                        ADB::null(),
                        ADB::null(),
                        { 1.1169, 1.0031, 0.0031 }} ) // the default magic numbers
        , terminal_output_ (terminal_output)
        , material_name_{ "Water", "Oil", "Gas" }
        , current_relaxation_(1.0)
    {
        assert(numMaterials() == 3); // Due to the material_name_ init above.
#if HAVE_MPI
        if ( linsolver_.parallelInformation().type() == typeid(ParallelISTLInformation) )
        {
            const ParallelISTLInformation& info =
                boost::any_cast<const ParallelISTLInformation&>(linsolver_.parallelInformation());
            if ( terminal_output_ ) {
                // Only rank 0 does print to std::cout if terminal_output is enabled
                terminal_output_ = (info.communicator().rank()==0);
            }
            int local_number_of_wells = wells_ ? wells_->number_of_wells : 0;
            int global_number_of_wells = info.communicator().sum(local_number_of_wells);
            wells_active_ = ( wells_ && global_number_of_wells > 0 );
            // Compute the global number of cells
            std::vector<int> v( Opm::AutoDiffGrid::numCells(grid_), 1);
            global_nc_ = 0;
            info.computeReduction(v, Opm::Reduction::makeGlobalSumFunctor<int>(), global_nc_);
        }else
#endif
        {
            wells_active_ = ( wells_ && wells_->number_of_wells > 0 );
            global_nc_    =  Opm::AutoDiffGrid::numCells(grid_);
        }
    }





    template <class Grid, class Implementation>
    void
    BlackoilModelBase<Grid, Implementation>::
    prepareStep(const double dt,
                ReservoirState& reservoir_state,
                WellState& /* well_state */)
    {
        pvdt_ = geo_.poreVolume() / dt;
        if (active_[Gas]) {
            updatePrimalVariableFromState(reservoir_state);
        }
    }





    template <class Grid, class Implementation>
    template <class NonlinearSolverType>
    IterationReport
    BlackoilModelBase<Grid, Implementation>::
    nonlinearIteration(const int iteration,
                       const double dt,
                       NonlinearSolverType& nonlinear_solver,
                       ReservoirState& reservoir_state,
                       WellState& well_state)
    {
        if (iteration == 0) {
            // For each iteration we store in a vector the norms of the residual of
            // the mass balance for each active phase, the well flux and the well equations.
            residual_norms_history_.clear();
            current_relaxation_ = 1.0;
            dx_old_ = V::Zero(sizeNonLinear());
        }
        asImpl().assemble(reservoir_state, well_state, iteration == 0);
        residual_norms_history_.push_back(asImpl().computeResidualNorms());
        const bool converged = asImpl().getConvergence(dt, iteration);
        const bool must_solve = (iteration < nonlinear_solver.minIter()) || (!converged);
        if (must_solve) {
            // Compute the nonlinear update.
            V dx = asImpl().solveJacobianSystem();

            // Stabilize the nonlinear update.
            bool isOscillate = false;
            bool isStagnate = false;
            nonlinear_solver.detectOscillations(residual_norms_history_, iteration, isOscillate, isStagnate);
            if (isOscillate) {
                current_relaxation_ -= nonlinear_solver.relaxIncrement();
                current_relaxation_ = std::max(current_relaxation_, nonlinear_solver.relaxMax());
                if (terminalOutputEnabled()) {
                    std::cout << " Oscillating behavior detected: Relaxation set to "
                              << current_relaxation_ << std::endl;
                }
            }
            nonlinear_solver.stabilizeNonlinearUpdate(dx, dx_old_, current_relaxation_);

            // Apply the update, applying model-dependent
            // limitations and chopping of the update.
            asImpl().updateState(dx, reservoir_state, well_state);
        }
        const bool failed = false; // Not needed in this model.
        const int linear_iters = must_solve ? asImpl().linearIterationsLastSolve() : 0;
        return IterationReport{ failed, converged, linear_iters };
    }





    template <class Grid, class Implementation>
    void
    BlackoilModelBase<Grid, Implementation>::
    afterStep(const double /* dt */,
              ReservoirState& /* reservoir_state */,
              WellState& /* well_state */)
    {
        // Does nothing in this model.
    }





    template <class Grid, class Implementation>
    int
    BlackoilModelBase<Grid, Implementation>::
    sizeNonLinear() const
    {
        return residual_.sizeNonLinear();
    }





    template <class Grid, class Implementation>
    int
    BlackoilModelBase<Grid, Implementation>::
    linearIterationsLastSolve() const
    {
        return linsolver_.iterations();
    }





    template <class Grid, class Implementation>
    bool
    BlackoilModelBase<Grid, Implementation>::
    terminalOutputEnabled() const
    {
        return terminal_output_;
    }





    template <class Grid, class Implementation>
    int
    BlackoilModelBase<Grid, Implementation>::
    numPhases() const
    {
        return fluid_.numPhases();
    }





    template <class Grid, class Implementation>
    int
    BlackoilModelBase<Grid, Implementation>::
    numMaterials() const
    {
        return material_name_.size();
    }





    template <class Grid, class Implementation>
    const std::string&
    BlackoilModelBase<Grid, Implementation>::
    materialName(int material_index) const
    {
        assert(material_index < numMaterials());
        return material_name_[material_index];
    }





    template <class Grid, class Implementation>
    void
    BlackoilModelBase<Grid, Implementation>::
    setThresholdPressures(const std::vector<double>& threshold_pressures_by_face)
    {
        const int num_faces = AutoDiffGrid::numFaces(grid_);
        if (int(threshold_pressures_by_face.size()) != num_faces) {
            OPM_THROW(std::runtime_error, "Illegal size of threshold_pressures_by_face input, must be equal to number of faces.");
        }
        use_threshold_pressure_ = true;
        // Map to interior faces.
        const int num_ifaces = ops_.internal_faces.size();
        threshold_pressures_by_interior_face_.resize(num_ifaces);
        for (int ii = 0; ii < num_ifaces; ++ii) {
            threshold_pressures_by_interior_face_[ii] = threshold_pressures_by_face[ops_.internal_faces[ii]];
        }
    }





    template <class Grid, class Implementation>
    BlackoilModelBase<Grid, Implementation>::ReservoirResidualQuant::ReservoirResidualQuant()
        : accum(2, ADB::null())
        , mflux(   ADB::null())
        , b    (   ADB::null())
        , dh   (   ADB::null())
        , mob  (   ADB::null())
    {
    }





    template <class Grid, class Implementation>
    BlackoilModelBase<Grid, Implementation>::
    WellOps::WellOps(const Wells* wells)
      : w2p(),
        p2w()
    {
        if( wells )
        {
            w2p = Eigen::SparseMatrix<double>(wells->well_connpos[ wells->number_of_wells ], wells->number_of_wells);
            p2w = Eigen::SparseMatrix<double>(wells->number_of_wells, wells->well_connpos[ wells->number_of_wells ]);

            const int        nw   = wells->number_of_wells;
            const int* const wpos = wells->well_connpos;

            typedef Eigen::Triplet<double> Tri;

            std::vector<Tri> scatter, gather;
            scatter.reserve(wpos[nw]);
            gather .reserve(wpos[nw]);

            for (int w = 0, i = 0; w < nw; ++w) {
                for (; i < wpos[ w + 1 ]; ++i) {
                    scatter.push_back(Tri(i, w, 1.0));
                    gather .push_back(Tri(w, i, 1.0));
                }
            }

            w2p.setFromTriplets(scatter.begin(), scatter.end());
            p2w.setFromTriplets(gather .begin(), gather .end());
        }
    }





    template <class Grid, class Implementation>
    void
    BlackoilModelBase<Grid, Implementation>::makeConstantState(SolutionState& state) const
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





    template <class Grid, class Implementation>
    typename BlackoilModelBase<Grid, Implementation>::SolutionState
    BlackoilModelBase<Grid, Implementation>::variableState(const ReservoirState& x,
                                                           const WellState&     xw) const
    {
        std::vector<V> vars0 = asImpl().variableStateInitials(x, xw);
        std::vector<ADB> vars = ADB::variables(vars0);
        return asImpl().variableStateExtractVars(x, asImpl().variableStateIndices(), vars);
    }





    template <class Grid, class Implementation>
    std::vector<V>
    BlackoilModelBase<Grid, Implementation>::variableStateInitials(const ReservoirState& x,
                                                                   const WellState&     xw) const
    {
        assert(active_[ Oil ]);

        const int np = x.numPhases();

        std::vector<V> vars0;
        // p, Sw and Rs, Rv or Sg is used as primary depending on solution conditions
        // and bhp and Q for the wells
        vars0.reserve(np + 1);
        variableReservoirStateInitials(x, vars0);
        asImpl().variableWellStateInitials(xw, vars0);
        return vars0;
    }





    template <class Grid, class Implementation>
    void
    BlackoilModelBase<Grid, Implementation>::variableReservoirStateInitials(const ReservoirState& x, std::vector<V>& vars0) const
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





    template <class Grid, class Implementation>
    void
    BlackoilModelBase<Grid, Implementation>::variableWellStateInitials(const WellState&     xw, std::vector<V>& vars0) const
    {
        // Initial well rates.
        if ( localWellsActive() )
        {
            // Need to reshuffle well rates, from phase running fastest
            // to wells running fastest.
            const int nw = wells().number_of_wells;
            const int np = wells().number_of_phases;

            // The transpose() below switches the ordering.
            const DataBlock wrates = Eigen::Map<const DataBlock>(& xw.wellRates()[0], nw, np).transpose();
            const V qs = Eigen::Map<const V>(wrates.data(), nw*np);
            vars0.push_back(qs);

            // Initial well bottom-hole pressure.
            assert (not xw.bhp().empty());
            const V bhp = Eigen::Map<const V>(& xw.bhp()[0], xw.bhp().size());
            vars0.push_back(bhp);
        }
        else
        {
            // push null states for qs and bhp
            vars0.push_back(V());
            vars0.push_back(V());
        }
    }





    template <class Grid, class Implementation>
    std::vector<int>
    BlackoilModelBase<Grid, Implementation>::variableStateIndices() const
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
        indices[Qs] = next++;
        indices[Bhp] = next++;
        assert(next == fluid_.numPhases() + 2);
        return indices;
    }




    template <class Grid, class Implementation>
    std::vector<int>
    BlackoilModelBase<Grid, Implementation>::variableWellStateIndices() const
    {
        // Black oil model standard is 5 equation.
        // For the pure well solve, only the well equations are picked.
        std::vector<int> indices(5, -1);
        int next = 0;
        indices[Qs] = next++;
        indices[Bhp] = next++;
        assert(next == 2);
        return indices;
    }





    template <class Grid, class Implementation>
    typename BlackoilModelBase<Grid, Implementation>::SolutionState
    BlackoilModelBase<Grid, Implementation>::variableStateExtractVars(const ReservoirState& x,
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

                if (active_[ Oil ]) {
                    // RS and RV is only defined if both oil and gas phase are active.
                    const ADB& sw = (active_[ Water ]
                                             ? state.saturation[ pu.phase_pos[ Water ] ]
                                             : ADB::constant(V::Zero(nc, 1)));
                    state.canonical_phase_pressures = computePressures(state.pressure, sw, so, sg);
                    const ADB rsSat = fluidRsSat(state.canonical_phase_pressures[ Oil ], so , cells_);
                    if (has_disgas_) {
                        state.rs = (1-isRs_)*rsSat + isRs_*xvar;
                    } else {
                        state.rs = rsSat;
                    }
                    const ADB rvSat = fluidRvSat(state.canonical_phase_pressures[ Gas ], so , cells_);
                    if (has_vapoil_) {
                        state.rv = (1-isRv_)*rvSat + isRv_*xvar;
                    } else {
                        state.rv = rvSat;
                    }
                }
            }

            if (active_[ Oil ]) {
                // Note that so is never a primary variable.
                state.saturation[pu.phase_pos[ Oil ]] = std::move(so);
            }
        }
        // wells
        asImpl().variableStateExtractWellsVars(indices, vars, state);
        return state;
    }





    template <class Grid, class Implementation>
    void
    BlackoilModelBase<Grid, Implementation>::variableStateExtractWellsVars(const std::vector<int>& indices,
                                                                          std::vector<ADB>& vars,
                                                                          SolutionState& state) const
    {
        // Qs.
        state.qs = std::move(vars[indices[Qs]]);

        // Bhp.
        state.bhp = std::move(vars[indices[Bhp]]);
    }





    template <class Grid, class Implementation>
    void
    BlackoilModelBase<Grid, Implementation>::computeAccum(const SolutionState& state,
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
                rq_[pos].b = fluidReciprocFVF(phase, state.canonical_phase_pressures[phase], temp, rs, rv, cond);
                rq_[pos].accum[aix] = pv_mult * rq_[pos].b * sat[pos];
                // OPM_AD_DUMP(rq_[pos].b);
                // OPM_AD_DUMP(rq_[pos].accum[aix]);
            }
        }

        if (active_[ Oil ] && active_[ Gas ]) {
            // Account for gas dissolved in oil and vaporized oil
            const int po = pu.phase_pos[ Oil ];
            const int pg = pu.phase_pos[ Gas ];

            // Temporary copy to avoid contribution of dissolved gas in the vaporized oil
            // when both dissolved gas and vaporized oil are present.
            const ADB accum_gas_copy =rq_[pg].accum[aix];

            rq_[pg].accum[aix] += state.rs * rq_[po].accum[aix];
            rq_[po].accum[aix] += state.rv * accum_gas_copy;
            // OPM_AD_DUMP(rq_[pg].accum[aix]);
        }
    }

    namespace detail {
        inline
        double getGravity(const double* g, const int dim) {
            double grav = 0.0;
            if (g) {
                // Guard against gravity in anything but last dimension.
                for (int dd = 0; dd < dim - 1; ++dd) {
                    assert(g[dd] == 0.0);
                }
                grav = g[dim - 1];
            }
            return grav;
        }
    }



    template <class Grid, class Implementation>
    void BlackoilModelBase<Grid, Implementation>::computeWellConnectionPressures(const SolutionState& state,
                                                                        const WellState& xw)
    {
        if( ! localWellsActive() ) return ;

        using namespace Opm::AutoDiffGrid;
        // 1. Compute properties required by computeConnectionPressureDelta().
        //    Note that some of the complexity of this part is due to the function
        //    taking std::vector<double> arguments, and not Eigen objects.
        const int nperf = wells().well_connpos[wells().number_of_wells];
        const int nw = wells().number_of_wells;
        const std::vector<int> well_cells(wells().well_cells, wells().well_cells + nperf);

        // Compute the average pressure in each well block
        const V perf_press = Eigen::Map<const V>(xw.perfPress().data(), nperf);
        V avg_press = perf_press*0;
        for (int w = 0; w < nw; ++w) {
            for (int perf = wells().well_connpos[w]; perf < wells().well_connpos[w+1]; ++perf) {
                const double p_above = perf == wells().well_connpos[w] ? state.bhp.value()[w] : perf_press[perf - 1];
                const double p_avg = (perf_press[perf] + p_above)/2;
                avg_press[perf] = p_avg;
            }
        }

#if 0
        std::cout << " state.bhp " << std::endl;
        std::cout << state.bhp.value() << std::endl;
        std::cout << " perf_press " << std::endl;
        std::cout << perf_press << std::endl;
        std::cout << " avg_press " << std::endl;
        std::cout << avg_press << std::endl;
#endif


        // Use cell values for the temperature as the wells don't knows its temperature yet.
        const ADB perf_temp = subset(state.temperature, well_cells);

        // Compute b, rsmax, rvmax values for perforations.
        // Evaluate the properties using average well block pressures
        // and cell values for rs, rv, phase condition and temperature.
        const ADB avg_press_ad = ADB::constant(avg_press);
        std::vector<PhasePresence> perf_cond(nperf);
        const std::vector<PhasePresence>& pc = phaseCondition();
        for (int perf = 0; perf < nperf; ++perf) {
            perf_cond[perf] = pc[well_cells[perf]];
        }
        const PhaseUsage& pu = fluid_.phaseUsage();
        DataBlock b(nperf, pu.num_phases);
        std::vector<double> rsmax_perf(nperf, 0.0);
        std::vector<double> rvmax_perf(nperf, 0.0);
        if (pu.phase_used[BlackoilPhases::Aqua]) {
            const V bw = fluid_.bWat(avg_press_ad, perf_temp, well_cells).value();
            b.col(pu.phase_pos[BlackoilPhases::Aqua]) = bw;
        }
        assert(active_[Oil]);
        const V perf_so =  subset(state.saturation[pu.phase_pos[Oil]].value(), well_cells);
        if (pu.phase_used[BlackoilPhases::Liquid]) {
            const ADB perf_rs = subset(state.rs, well_cells);
            const V bo = fluid_.bOil(avg_press_ad, perf_temp, perf_rs, perf_cond, well_cells).value();
            b.col(pu.phase_pos[BlackoilPhases::Liquid]) = bo;
            const V rssat = fluidRsSat(avg_press, perf_so, well_cells);
            rsmax_perf.assign(rssat.data(), rssat.data() + nperf);
        }
        if (pu.phase_used[BlackoilPhases::Vapour]) {
            const ADB perf_rv = subset(state.rv, well_cells);
            const V bg = fluid_.bGas(avg_press_ad, perf_temp, perf_rv, perf_cond, well_cells).value();
            b.col(pu.phase_pos[BlackoilPhases::Vapour]) = bg;
            const V rvsat = fluidRvSat(avg_press, perf_so, well_cells);
            rvmax_perf.assign(rvsat.data(), rvsat.data() + nperf);
        }
        // b is row major, so can just copy data.
        std::vector<double> b_perf(b.data(), b.data() + nperf * pu.num_phases);
        // Extract well connection depths.
        const V depth = cellCentroidsZToEigen(grid_);
        const V pdepth = subset(depth, well_cells);
        std::vector<double> perf_depth(pdepth.data(), pdepth.data() + nperf);

        // Surface density.
        // The compute density segment wants the surface densities as
        // an np * number of wells cells array
        V rho = superset(fluid_.surfaceDensity(0 , well_cells), Span(nperf, pu.num_phases, 0), nperf*pu.num_phases);
        for (int phase = 1; phase < pu.num_phases; ++phase) {
            rho += superset(fluid_.surfaceDensity(phase , well_cells), Span(nperf, pu.num_phases, phase), nperf*pu.num_phases);
        }
        std::vector<double> surf_dens_perf(rho.data(), rho.data() + nperf * pu.num_phases);

        // Gravity
        double grav = detail::getGravity(geo_.gravity(), dimensions(grid_));

        // 2. Compute densities
        std::vector<double> cd =
                WellDensitySegmented::computeConnectionDensities(
                        wells(), xw, fluid_.phaseUsage(),
                        b_perf, rsmax_perf, rvmax_perf, surf_dens_perf);

        // 3. Compute pressure deltas
        std::vector<double> cdp =
                WellDensitySegmented::computeConnectionPressureDelta(
                        wells(), perf_depth, cd, grav);

        // 4. Store the results
        well_perforation_densities_ = Eigen::Map<const V>(cd.data(), nperf);
        well_perforation_pressure_diffs_ = Eigen::Map<const V>(cdp.data(), nperf);
#if 0
        std::cout << "well_perforation_densities_ " << std::endl;
        std::cout << well_perforation_densities_ << std::endl;

        std::cout << "well_perforation_pressure_diffs_ " << std::endl;
        std::cout << well_perforation_pressure_diffs_ << std::endl;
#endif
    }





    template <class Grid, class Implementation>
    void
    BlackoilModelBase<Grid, Implementation>::
    assemble(const ReservoirState& reservoir_state,
             WellState& well_state,
             const bool initial_assembly)
    {
        using namespace Opm::AutoDiffGrid;

        // If we have VFP tables, we need the well connection
        // pressures for the "simple" hydrostatic correction
        // between well depth and vfp table depth.
        if (isVFPActive()) {
            SolutionState state = asImpl().variableState(reservoir_state, well_state);
            SolutionState state0 = state;
            asImpl().makeConstantState(state0);
            asImpl().computeWellConnectionPressures(state0, well_state);
        }

        // Possibly switch well controls and updating well state to
        // get reasonable initial conditions for the wells
        asImpl().updateWellControls(well_state);

        // Create the primary variables.
        SolutionState state = asImpl().variableState(reservoir_state, well_state);

        if (initial_assembly) {
            // Create the (constant, derivativeless) initial state.
            SolutionState state0 = state;
            asImpl().makeConstantState(state0);
            // Compute initial accumulation contributions
            // and well connection pressures.
            asImpl().computeAccum(state0, 0);
            asImpl().computeWellConnectionPressures(state0, well_state);
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
            return;
        }

        V aliveWells;
        const int np = wells().number_of_phases;
        std::vector<ADB> cq_s(np, ADB::null());

        const int nw = wells().number_of_wells;
        const int nperf = wells().well_connpos[nw];
        const std::vector<int> well_cells(wells().well_cells, wells().well_cells + nperf);

        std::vector<ADB> mob_perfcells(np, ADB::null());
        std::vector<ADB> b_perfcells(np, ADB::null());
        for (int phase = 0; phase < np; ++phase) {
            mob_perfcells[phase] = subset(rq_[phase].mob, well_cells);
            b_perfcells[phase] = subset(rq_[phase].b, well_cells);
        }
        if (param_.solve_welleq_initially_ && initial_assembly) {
            // solve the well equations as a pre-processing step
            solveWellEq(mob_perfcells, b_perfcells, state, well_state);
        }

        asImpl().computeWellFlux(state, mob_perfcells, b_perfcells, aliveWells, cq_s);
        asImpl().updatePerfPhaseRatesAndPressures(cq_s, state, well_state);
        asImpl().addWellFluxEq(cq_s, state);
        asImpl().addWellContributionToMassBalanceEq(cq_s, state, well_state);
        asImpl().addWellControlEq(state, well_state, aliveWells);
    }





    template <class Grid, class Implementation>
    void
    BlackoilModelBase<Grid, Implementation>::
    assembleMassBalanceEq(const SolutionState& state)
    {
        // Compute b_p and the accumulation term b_p*s_p for each phase,
        // except gas. For gas, we compute b_g*s_g + Rs*b_o*s_o.
        // These quantities are stored in rq_[phase].accum[1].
        // The corresponding accumulation terms from the start of
        // the timestep (b^0_p*s^0_p etc.) were already computed
        // on the initial call to assemble() and stored in rq_[phase].accum[0].
        asImpl().computeAccum(state, 1);

        // Set up the common parts of the mass balance equations
        // for each active phase.
        const V transi = subset(geo_.transmissibility(), ops_.internal_faces);
        const V trans_nnc = ops_.nnc_trans;
        V trans_all(transi.size() + trans_nnc.size());
        trans_all << transi, trans_nnc;

        const std::vector<ADB> kr = asImpl().computeRelPerm(state);
        for (int phaseIdx = 0; phaseIdx < fluid_.numPhases(); ++phaseIdx) {
            asImpl().computeMassFlux(phaseIdx, trans_all, kr[canph_[phaseIdx]], state.canonical_phase_pressures[canph_[phaseIdx]], state);

            residual_.material_balance_eq[ phaseIdx ] =
                pvdt_ * (rq_[phaseIdx].accum[1] - rq_[phaseIdx].accum[0])
                + ops_.div*rq_[phaseIdx].mflux;
        }

        // -------- Extra (optional) rs and rv contributions to the mass balance equations --------

        // Add the extra (flux) terms to the mass balance equations
        // From gas dissolved in the oil phase (rs) and oil vaporized in the gas phase (rv)
        // The extra terms in the accumulation part of the equation are already handled.
        if (active_[ Oil ] && active_[ Gas ]) {
            const int po = fluid_.phaseUsage().phase_pos[ Oil ];
            const int pg = fluid_.phaseUsage().phase_pos[ Gas ];

            const UpwindSelector<double> upwindOil(grid_, ops_,
                                                rq_[po].dh.value());
            const ADB rs_face = upwindOil.select(state.rs);

            const UpwindSelector<double> upwindGas(grid_, ops_,
                                                rq_[pg].dh.value());
            const ADB rv_face = upwindGas.select(state.rv);

            residual_.material_balance_eq[ pg ] += ops_.div * (rs_face * rq_[po].mflux);
            residual_.material_balance_eq[ po ] += ops_.div * (rv_face * rq_[pg].mflux);

            // OPM_AD_DUMP(residual_.material_balance_eq[ Gas ]);

        }


        if (param_.update_equations_scaling_) {
            asImpl().updateEquationsScaling();
        }

    }





    template <class Grid, class Implementation>
    void
    BlackoilModelBase<Grid, Implementation>::updateEquationsScaling() {
        ADB::V B;
        const Opm::PhaseUsage& pu = fluid_.phaseUsage();
        for ( int idx=0; idx<MaxNumPhases; ++idx )
        {
            if (active_[idx]) {
                const int pos    = pu.phase_pos[idx];
                const ADB& temp_b = rq_[pos].b;
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





    template <class Grid, class Implementation>
    void
    BlackoilModelBase<Grid, Implementation>::addWellContributionToMassBalanceEq(const std::vector<ADB>& cq_s,
                                                                                const SolutionState&,
                                                                                const WellState&)
    {
        // Add well contributions to mass balance equations
        const int nc = Opm::AutoDiffGrid::numCells(grid_);
        const int nw = wells().number_of_wells;
        const int nperf = wells().well_connpos[nw];
        const int np = wells().number_of_phases;
        const std::vector<int> well_cells(wells().well_cells, wells().well_cells + nperf);
        for (int phase = 0; phase < np; ++phase) {
            residual_.material_balance_eq[phase] -= superset(cq_s[phase], well_cells, nc);
        }
    }




    template <class Grid, class Implementation>
    void
    BlackoilModelBase<Grid, Implementation>::computeWellFlux(const SolutionState& state,
                                                             const std::vector<ADB>& mob_perfcells,
                                                             const std::vector<ADB>& b_perfcells,
                                                             V& aliveWells,
                                                             std::vector<ADB>& cq_s)
    {
        if( ! localWellsActive() ) return ;

        const int np = wells().number_of_phases;
        const int nw = wells().number_of_wells;
        const int nperf = wells().well_connpos[nw];
        const Opm::PhaseUsage& pu = fluid_.phaseUsage();
        V Tw = Eigen::Map<const V>(wells().WI, nperf);
        const std::vector<int> well_cells(wells().well_cells, wells().well_cells + nperf);

        // pressure diffs computed already (once per step, not changing per iteration)
        const V& cdp = well_perforation_pressure_diffs_;
        // Extract needed quantities for the perforation cells
        const ADB& p_perfcells = subset(state.pressure, well_cells);
        const ADB& rv_perfcells = subset(state.rv, well_cells);
        const ADB& rs_perfcells = subset(state.rs, well_cells);

        // Perforation pressure
        const ADB perfpressure = (wops_.w2p * state.bhp) + cdp;

        // Pressure drawdown (also used to determine direction of flow)
        const ADB drawdown =  p_perfcells - perfpressure;

        // Compute vectors with zero and ones that
        // selects the wanted quantities.

        // selects injection perforations
        V selectInjectingPerforations = V::Zero(nperf);
        // selects producing perforations
        V selectProducingPerforations = V::Zero(nperf);
        for (int c = 0; c < nperf; ++c){
            if (drawdown.value()[c] < 0)
                selectInjectingPerforations[c] = 1;
            else
                selectProducingPerforations[c] = 1;
        }

        // Handle cross flow
        const V numInjectingPerforations = (wops_.p2w * ADB::constant(selectInjectingPerforations)).value();
        const V numProducingPerforations = (wops_.p2w * ADB::constant(selectProducingPerforations)).value();
        for (int w = 0; w < nw; ++w) {
            if (!wells().allow_cf[w]) {
                for (int perf = wells().well_connpos[w] ; perf < wells().well_connpos[w+1]; ++perf) {
                    // Crossflow is not allowed; reverse flow is prevented.
                    // At least one of the perforation must be open in order to have a meeningful
                    // equation to solve. For the special case where all perforations have reverse flow,
                    // and the target rate is non-zero all of the perforations are keept open.
                    if (wells().type[w] == INJECTOR && numInjectingPerforations[w] > 0) {
                        selectProducingPerforations[perf] = 0.0;
                    } else if (wells().type[w] == PRODUCER && numProducingPerforations[w] > 0 ){
                        selectInjectingPerforations[perf] = 0.0;
                    }
                }
            }
        }

        // HANDLE FLOW INTO WELLBORE
        // compute phase volumetric rates at standard conditions
        std::vector<ADB> cq_ps(np, ADB::null());
        for (int phase = 0; phase < np; ++phase) {
            const ADB cq_p = -(selectProducingPerforations * Tw) * (mob_perfcells[phase] * drawdown);
            cq_ps[phase] = b_perfcells[phase] * cq_p;
        }
        if (active_[Oil] && active_[Gas]) {
            const int oilpos = pu.phase_pos[Oil];
            const int gaspos = pu.phase_pos[Gas];
            const ADB cq_psOil = cq_ps[oilpos];
            const ADB cq_psGas = cq_ps[gaspos];
            cq_ps[gaspos] += rs_perfcells * cq_psOil;
            cq_ps[oilpos] += rv_perfcells * cq_psGas;
        }

        // HANDLE FLOW OUT FROM WELLBORE
        // Using total mobilities
        ADB total_mob = mob_perfcells[0];
        for (int phase = 1; phase < np; ++phase) {
            total_mob += mob_perfcells[phase];
        }
        // injection perforations total volume rates
        const ADB cqt_i = -(selectInjectingPerforations * Tw) * (total_mob * drawdown);

        // compute wellbore mixture for injecting perforations
        // The wellbore mixture depends on the inflow from the reservoar
        // and the well injection rates.

        // compute avg. and total wellbore phase volumetric rates at standard conds
        const DataBlock compi = Eigen::Map<const DataBlock>(wells().comp_frac, nw, np);
        std::vector<ADB> wbq(np, ADB::null());
        ADB wbqt = ADB::constant(V::Zero(nw));
        for (int phase = 0; phase < np; ++phase) {
            const ADB& q_ps = wops_.p2w * cq_ps[phase];
            const ADB& q_s = subset(state.qs, Span(nw, 1, phase*nw));
            Selector<double> injectingPhase_selector(q_s.value(), Selector<double>::GreaterZero);
            const int pos = pu.phase_pos[phase];
            wbq[phase] = (compi.col(pos) * injectingPhase_selector.select(q_s,ADB::constant(V::Zero(nw))))  - q_ps;
            wbqt += wbq[phase];
        }
        // compute wellbore mixture at standard conditions.
        Selector<double> notDeadWells_selector(wbqt.value(), Selector<double>::Zero);
        std::vector<ADB> cmix_s(np, ADB::null());
        for (int phase = 0; phase < np; ++phase) {
            const int pos = pu.phase_pos[phase];
            cmix_s[phase] = wops_.w2p * notDeadWells_selector.select(ADB::constant(compi.col(pos)), wbq[phase]/wbqt);
        }

        // compute volume ratio between connection at standard conditions
        ADB volumeRatio = ADB::constant(V::Zero(nperf));
        const ADB d = V::Constant(nperf,1.0) -  rv_perfcells * rs_perfcells;
        for (int phase = 0; phase < np; ++phase) {
            ADB tmp = cmix_s[phase];

            if (phase == Oil && active_[Gas]) {
                const int gaspos = pu.phase_pos[Gas];
                tmp = tmp - rv_perfcells * cmix_s[gaspos] / d;
            }
            if (phase == Gas && active_[Oil]) {
                const int oilpos = pu.phase_pos[Oil];
                tmp = tmp - rs_perfcells * cmix_s[oilpos] / d;
            }
            volumeRatio += tmp / b_perfcells[phase];
        }

        // injecting connections total volumerates at standard conditions
        ADB cqt_is = cqt_i/volumeRatio;

        // connection phase volumerates at standard conditions
        for (int phase = 0; phase < np; ++phase) {
            cq_s[phase] = cq_ps[phase] + cmix_s[phase]*cqt_is;
        }

        // check for dead wells (used in the well controll equations)
        aliveWells = V::Constant(nw, 1.0);
        for (int w = 0; w < nw; ++w) {
            if (wbqt.value()[w] == 0) {
                aliveWells[w] = 0.0;
            }
        }
    }





    template <class Grid, class Implementation>
    void BlackoilModelBase<Grid, Implementation>::updatePerfPhaseRatesAndPressures(const std::vector<ADB>& cq_s,
                                                                                   const SolutionState& state,
                                                                                   WellState& xw)
    {
        // Update the perforation phase rates (used to calculate the pressure drop in the wellbore).
        const int np = wells().number_of_phases;
        const int nw = wells().number_of_wells;
        const int nperf = wells().well_connpos[nw];
        V cq = superset(cq_s[0].value(), Span(nperf, np, 0), nperf*np);
        for (int phase = 1; phase < np; ++phase) {
            cq += superset(cq_s[phase].value(), Span(nperf, np, phase), nperf*np);
        }
        xw.perfPhaseRates().assign(cq.data(), cq.data() + nperf*np);

        // Update the perforation pressures.
        const V& cdp = well_perforation_pressure_diffs_;
        const V perfpressure = (wops_.w2p * state.bhp.value().matrix()).array() + cdp;
        xw.perfPress().assign(perfpressure.data(), perfpressure.data() + nperf);
    }





    template <class Grid, class Implementation>
    void BlackoilModelBase<Grid, Implementation>::addWellFluxEq(const std::vector<ADB>& cq_s,
                                                                const SolutionState& state)
    {
        const int np = wells().number_of_phases;
        const int nw = wells().number_of_wells;
        ADB qs = state.qs;
        for (int phase = 0; phase < np; ++phase) {
            qs -= superset(wops_.p2w * cq_s[phase], Span(nw, 1, phase*nw), nw*np);

        }

        residual_.well_flux_eq = qs;
    }





    namespace detail
    {
        inline
        double rateToCompare(const std::vector<double>& well_phase_flow_rate,
                             const int well,
                             const int num_phases,
                             const double* distr)
        {
            double rate = 0.0;
            for (int phase = 0; phase < num_phases; ++phase) {
                // Important: well_phase_flow_rate is ordered with all phase rates for first
                // well first, then all phase rates for second well etc.
                rate += well_phase_flow_rate[well*num_phases + phase] * distr[phase];
            }
            return rate;
        }

        inline
        bool constraintBroken(const std::vector<double>& bhp,
                              const std::vector<double>& thp,
                              const std::vector<double>& well_phase_flow_rate,
                              const int well,
                              const int num_phases,
                              const WellType& well_type,
                              const WellControls* wc,
                              const int ctrl_index)
        {
            const WellControlType ctrl_type = well_controls_iget_type(wc, ctrl_index);
            const double target = well_controls_iget_target(wc, ctrl_index);
            const double* distr = well_controls_iget_distr(wc, ctrl_index);

            bool broken = false;

            switch (well_type) {
            case INJECTOR:
            {
                switch (ctrl_type) {
                case BHP:
                    broken = bhp[well] > target;
                    break;

                case THP:
                    broken = thp[well] > target;
                    break;

                case RESERVOIR_RATE: // Intentional fall-through
                case SURFACE_RATE:
                    broken = rateToCompare(well_phase_flow_rate,
                                           well, num_phases, distr) > target;
                    break;
                }
            }
            break;

            case PRODUCER:
            {
                switch (ctrl_type) {
                case BHP:
                    broken = bhp[well] < target;
                    break;

                case THP:
                    broken = thp[well] < target;
                    break;

                case RESERVOIR_RATE: // Intentional fall-through
                case SURFACE_RATE:
                    // Note that the rates compared below are negative,
                    // so breaking the constraints means: too high flow rate
                    // (as for injection).
                    broken = rateToCompare(well_phase_flow_rate,
                                           well, num_phases, distr) < target;
                    break;
                }
            }
            break;

            default:
                OPM_THROW(std::logic_error, "Can only handle INJECTOR and PRODUCER wells.");
            }

            return broken;
        }
    } // namespace detail


    namespace detail {
        /**
         * Simple hydrostatic correction for VFP table
         * @param wells - wells struct
         * @param w Well number
         * @param vfp_table VFP table
         * @param well_perforation_densities Densities at well perforations
         * @param gravity Gravitational constant (e.g., 9.81...)
         */
        inline
        double computeHydrostaticCorrection(const Wells& wells, const int w, double vfp_ref_depth,
                                            const ADB::V& well_perforation_densities, const double gravity) {
            if ( wells.well_connpos[w] == wells.well_connpos[w+1] )
            {
                // This is a well with no perforations.
                // If this is the last well we would subscript over the
                // bounds below.
                // we assume well_perforation_densities to be 0
                return 0;
            }
            const double well_ref_depth = wells.depth_ref[w];
            const double dh = vfp_ref_depth - well_ref_depth;
            const int perf = wells.well_connpos[w];
            const double rho = well_perforation_densities[perf];
            const double dp = rho*gravity*dh;

            return dp;
        }

        inline
        ADB::V computeHydrostaticCorrection(const Wells& wells, const ADB::V vfp_ref_depth,
                const ADB::V& well_perforation_densities, const double gravity) {
            const int nw = wells.number_of_wells;
            ADB::V retval = ADB::V::Zero(nw);

#pragma omp parallel for schedule(static)
            for (int i=0; i<nw; ++i) {
                retval[i] = computeHydrostaticCorrection(wells, i, vfp_ref_depth[i], well_perforation_densities, gravity);
            }

            return retval;
        }

    } //Namespace


    template <class Grid, class Implementation>
    bool BlackoilModelBase<Grid, Implementation>::isVFPActive() const
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


    template <class Grid, class Implementation>
    void BlackoilModelBase<Grid, Implementation>::updateWellControls(WellState& xw) const
    {
        if( ! localWellsActive() ) return ;

        std::string modestring[4] = { "BHP", "THP", "RESERVOIR_RATE", "SURFACE_RATE" };
        // Find, for each well, if any constraints are broken. If so,
        // switch control to first broken constraint.
        const int np = wells().number_of_phases;
        const int nw = wells().number_of_wells;
        const Opm::PhaseUsage& pu = fluid_.phaseUsage();
#pragma omp parallel for schedule(dynamic)
        for (int w = 0; w < nw; ++w) {
            const WellControls* wc = wells().ctrls[w];
            // The current control in the well state overrides
            // the current control set in the Wells struct, which
            // is instead treated as a default.
            int current = xw.currentControls()[w];
            // Loop over all controls except the current one, and also
            // skip any RESERVOIR_RATE controls, since we cannot
            // handle those.
            const int nwc = well_controls_get_num(wc);
            int ctrl_index = 0;
            for (; ctrl_index < nwc; ++ctrl_index) {
                if (ctrl_index == current) {
                    // This is the currently used control, so it is
                    // used as an equation. So this is not used as an
                    // inequality constraint, and therefore skipped.
                    continue;
                }
                if (detail::constraintBroken(
                        xw.bhp(), xw.thp(), xw.wellRates(),
                        w, np, wells().type[w], wc, ctrl_index)) {
                    // ctrl_index will be the index of the broken constraint after the loop.
                    break;
                }
            }
            if (ctrl_index != nwc) {
                // Constraint number ctrl_index was broken, switch to it.
                if (terminal_output_)
                {
                    std::cout << "Switching control mode for well " << wells().name[w]
                              << " from " << modestring[well_controls_iget_type(wc, current)]
                              << " to " << modestring[well_controls_iget_type(wc, ctrl_index)] << std::endl;
                }
                xw.currentControls()[w] = ctrl_index;
                current = xw.currentControls()[w];
            }

            //Get gravity for THP hydrostatic corrrection
            const double gravity = detail::getGravity(geo_.gravity(), UgGridHelpers::dimensions(grid_));

            // Updating well state and primary variables.
            // Target values are used as initial conditions for BHP, THP, and SURFACE_RATE
            const double target = well_controls_iget_target(wc, current);
            const double* distr = well_controls_iget_distr(wc, current);
            switch (well_controls_iget_type(wc, current)) {
            case BHP:
                xw.bhp()[w] = target;
                break;

            case THP: {
                double aqua = 0.0;
                double liquid = 0.0;
                double vapour = 0.0;

                if (active_[ Water ]) {
                    aqua = xw.wellRates()[w*np + pu.phase_pos[ Water ] ];
                }
                if (active_[ Oil ]) {
                    liquid = xw.wellRates()[w*np + pu.phase_pos[ Oil ] ];
                }
                if (active_[ Gas ]) {
                    vapour = xw.wellRates()[w*np + pu.phase_pos[ Gas ] ];
                }

                const int vfp        = well_controls_iget_vfp(wc, current);
                const double& thp    = well_controls_iget_target(wc, current);
                const double& alq    = well_controls_iget_alq(wc, current);

                //Set *BHP* target by calculating bhp from THP
                const WellType& well_type = wells().type[w];

                if (well_type == INJECTOR) {
                    double dp = detail::computeHydrostaticCorrection(
                            wells(), w, vfp_properties_.getInj()->getTable(vfp)->getDatumDepth(),
                            well_perforation_densities_, gravity);

                    xw.bhp()[w] = vfp_properties_.getInj()->bhp(vfp, aqua, liquid, vapour, thp) - dp;
                }
                else if (well_type == PRODUCER) {
                    double dp = detail::computeHydrostaticCorrection(
                            wells(), w, vfp_properties_.getProd()->getTable(vfp)->getDatumDepth(),
                            well_perforation_densities_, gravity);

                    xw.bhp()[w] = vfp_properties_.getProd()->bhp(vfp, aqua, liquid, vapour, thp, alq) - dp;
                }
                else {
                    OPM_THROW(std::logic_error, "Expected PRODUCER or INJECTOR type of well");
                }
                break;
            }

            case RESERVOIR_RATE:
                // No direct change to any observable quantity at
                // surface condition.  In this case, use existing
                // flow rates as initial conditions as reservoir
                // rate acts only in aggregate.
                break;

            case SURFACE_RATE:
                for (int phase = 0; phase < np; ++phase) {
                    if (distr[phase] > 0.0) {
                        xw.wellRates()[np*w + phase] = target * distr[phase];
                    }
                }
                break;
            }

        }
    }





    template <class Grid, class Implementation>
    void BlackoilModelBase<Grid, Implementation>::solveWellEq(const std::vector<ADB>& mob_perfcells,
                                                              const std::vector<ADB>& b_perfcells,
                                                              SolutionState& state,
                                                              WellState& well_state)
    {
        V aliveWells;
        const int np = wells().number_of_phases;
        std::vector<ADB> cq_s(np, ADB::null());
        std::vector<int> indices = variableWellStateIndices();
        SolutionState state0 = state;
        asImpl().makeConstantState(state0);

        std::vector<ADB> mob_perfcells_const(np, ADB::null());
        std::vector<ADB> b_perfcells_const(np, ADB::null());
        for (int phase = 0; phase < np; ++phase) {
            mob_perfcells_const[phase] = ADB::constant(mob_perfcells[phase].value());
            b_perfcells_const[phase] = ADB::constant(b_perfcells[phase].value());
        }

        int it  = 0;
        bool converged;
        do {
            // bhp and Q for the wells
            std::vector<V> vars0;
            vars0.reserve(2);
            asImpl().variableWellStateInitials(well_state, vars0);
            std::vector<ADB> vars = ADB::variables(vars0);

            SolutionState wellSolutionState = state0;
            asImpl().variableStateExtractWellsVars(indices, vars, wellSolutionState);
            asImpl().computeWellFlux(wellSolutionState, mob_perfcells_const, b_perfcells_const, aliveWells, cq_s);
            asImpl().updatePerfPhaseRatesAndPressures(cq_s, wellSolutionState, well_state);
            asImpl().addWellFluxEq(cq_s, wellSolutionState);
            asImpl().addWellControlEq(wellSolutionState, well_state, aliveWells);
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
                assert(dx.size() == (well_state.numWells() * (well_state.numPhases()+1)));
                asImpl().updateWellState(dx.array(), well_state);
                asImpl().updateWellControls(well_state);
            }
        } while (it < 15);

        if (converged) {
            if ( terminal_output_ ) {
                std::cout << "well converged iter: " << it << std::endl;
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

    }





    template <class Grid, class Implementation>
    void BlackoilModelBase<Grid, Implementation>::addWellControlEq(const SolutionState& state,
                                                          const WellState& xw,
                                                          const V& aliveWells)
    {
        if( ! localWellsActive() ) return;

        const int np = wells().number_of_phases;
        const int nw = wells().number_of_wells;

        ADB aqua   = ADB::constant(ADB::V::Zero(nw));
        ADB liquid = ADB::constant(ADB::V::Zero(nw));
        ADB vapour = ADB::constant(ADB::V::Zero(nw));

        if (active_[Water]) {
            aqua += subset(state.qs, Span(nw, 1, BlackoilPhases::Aqua*nw));
        }
        if (active_[Oil]) {
            liquid += subset(state.qs, Span(nw, 1, BlackoilPhases::Liquid*nw));
        }
        if (active_[Gas]) {
            vapour += subset(state.qs, Span(nw, 1, BlackoilPhases::Vapour*nw));
        }

        //THP calculation variables
        std::vector<int> inj_table_id(nw, -1);
        std::vector<int> prod_table_id(nw, -1);
        ADB::V thp_inj_target_v = ADB::V::Zero(nw);
        ADB::V thp_prod_target_v = ADB::V::Zero(nw);
        ADB::V alq_v = ADB::V::Zero(nw);

        //Hydrostatic correction variables
        ADB::V rho_v = ADB::V::Zero(nw);
        ADB::V vfp_ref_depth_v = ADB::V::Zero(nw);

        //Target vars
        ADB::V bhp_targets  = ADB::V::Zero(nw);
        ADB::V rate_targets = ADB::V::Zero(nw);
        Eigen::SparseMatrix<double> rate_distr(nw, np*nw);

        //Selection variables
        std::vector<int> bhp_elems;
        std::vector<int> thp_inj_elems;
        std::vector<int> thp_prod_elems;
        std::vector<int> rate_elems;

        //Run through all wells to calculate BHP/RATE targets
        //and gather info about current control
        for (int w = 0; w < nw; ++w) {
            auto wc = wells().ctrls[w];

            // The current control in the well state overrides
            // the current control set in the Wells struct, which
            // is instead treated as a default.
            const int current = xw.currentControls()[w];

            switch (well_controls_iget_type(wc, current)) {
            case BHP:
            {
                bhp_elems.push_back(w);
                bhp_targets(w)  = well_controls_iget_target(wc, current);
                rate_targets(w) = -1e100;
            }
            break;

            case THP:
            {
                const int perf = wells().well_connpos[w];
                rho_v[w] = well_perforation_densities_[perf];

                const int table_id = well_controls_iget_vfp(wc, current);
                const double target = well_controls_iget_target(wc, current);

                const WellType& well_type = wells().type[w];
                if (well_type == INJECTOR) {
                    inj_table_id[w]  = table_id;
                    thp_inj_target_v[w] = target;
                    alq_v[w]     = -1e100;

                    vfp_ref_depth_v[w] = vfp_properties_.getInj()->getTable(table_id)->getDatumDepth();

                    thp_inj_elems.push_back(w);
                }
                else if (well_type == PRODUCER) {
                    prod_table_id[w]  = table_id;
                    thp_prod_target_v[w] = target;
                    alq_v[w]      = well_controls_iget_alq(wc, current);

                    vfp_ref_depth_v[w] =  vfp_properties_.getProd()->getTable(table_id)->getDatumDepth();

                    thp_prod_elems.push_back(w);
                }
                else {
                    OPM_THROW(std::logic_error, "Expected INJECTOR or PRODUCER type well");
                }
                bhp_targets(w)  = -1e100;
                rate_targets(w) = -1e100;
            }
            break;

            case RESERVOIR_RATE: // Intentional fall-through
            case SURFACE_RATE:
            {
                rate_elems.push_back(w);
                // RESERVOIR and SURFACE rates look the same, from a
                // high-level point of view, in the system of
                // simultaneous linear equations.

                const double* const distr =
                    well_controls_iget_distr(wc, current);

                for (int p = 0; p < np; ++p) {
                    rate_distr.insert(w, p*nw + w) = distr[p];
                }

                bhp_targets(w)  = -1.0e100;
                rate_targets(w) = well_controls_iget_target(wc, current);
            }
            break;
            }
        }

        //Calculate BHP target from THP
        const ADB thp_inj_target = ADB::constant(thp_inj_target_v);
        const ADB thp_prod_target = ADB::constant(thp_prod_target_v);
        const ADB alq = ADB::constant(alq_v);
        const ADB bhp_from_thp_inj = vfp_properties_.getInj()->bhp(inj_table_id, aqua, liquid, vapour, thp_inj_target);
        const ADB bhp_from_thp_prod = vfp_properties_.getProd()->bhp(prod_table_id, aqua, liquid, vapour, thp_prod_target, alq);

        //Perform hydrostatic correction to computed targets
        double gravity = detail::getGravity(geo_.gravity(), UgGridHelpers::dimensions(grid_));
        const ADB::V dp_v = detail::computeHydrostaticCorrection(wells(), vfp_ref_depth_v, well_perforation_densities_, gravity);
        const ADB dp = ADB::constant(dp_v);
        const ADB dp_inj = superset(subset(dp, thp_inj_elems), thp_inj_elems, nw);
        const ADB dp_prod = superset(subset(dp, thp_prod_elems), thp_prod_elems, nw);

        //Calculate residuals
        const ADB thp_inj_residual = state.bhp - bhp_from_thp_inj + dp_inj;
        const ADB thp_prod_residual = state.bhp - bhp_from_thp_prod + dp_prod;
        const ADB bhp_residual = state.bhp - bhp_targets;
        const ADB rate_residual = rate_distr * state.qs - rate_targets;

        //Select the right residual for each well
        residual_.well_eq = superset(subset(bhp_residual, bhp_elems), bhp_elems, nw) +
                superset(subset(thp_inj_residual, thp_inj_elems), thp_inj_elems, nw) +
                superset(subset(thp_prod_residual, thp_prod_elems), thp_prod_elems, nw) +
                superset(subset(rate_residual, rate_elems), rate_elems, nw);

        // For wells that are dead (not flowing), and therefore not communicating
        // with the reservoir, we set the equation to be equal to the well's total
        // flow. This will be a solution only if the target rate is also zero.
        Eigen::SparseMatrix<double> rate_summer(nw, np*nw);
        for (int w = 0; w < nw; ++w) {
            for (int phase = 0; phase < np; ++phase) {
                rate_summer.insert(w, phase*nw + w) = 1.0;
            }
        }
        Selector<double> alive_selector(aliveWells, Selector<double>::NotEqualZero);
        residual_.well_eq = alive_selector.select(residual_.well_eq, rate_summer * state.qs);
        // OPM_AD_DUMP(residual_.well_eq);
    }





    template <class Grid, class Implementation>
    V BlackoilModelBase<Grid, Implementation>::solveJacobianSystem() const
    {
        return linsolver_.computeNewtonIncrement(residual_);
    }





    namespace detail
    {
        /// \brief Compute the L-infinity norm of a vector
        /// \warn This function is not suitable to compute on the well equations.
        /// \param a The container to compute the infinity norm on.
        ///          It has to have one entry for each cell.
        /// \param info In a parallel this holds the information about the data distribution.
        inline
        double infinityNorm( const ADB& a, const boost::any& pinfo = boost::any() )
        {
            static_cast<void>(pinfo); // Suppress warning in non-MPI case.
#if HAVE_MPI
            if ( pinfo.type() == typeid(ParallelISTLInformation) )
            {
                const ParallelISTLInformation& real_info =
                    boost::any_cast<const ParallelISTLInformation&>(pinfo);
                double result=0;
                real_info.computeReduction(a.value(), Reduction::makeGlobalMaxFunctor<double>(), result);
                return result;
            }
            else
#endif
            {
                if( a.value().size() > 0 ) {
                    return a.value().matrix().lpNorm<Eigen::Infinity> ();
                }
                else { // this situation can occur when no wells are present
                    return 0.0;
                }
            }
        }

        /// \brief Compute the Euclidian norm of a vector
        /// \param it              begin iterator for the given vector
        /// \param end             end iterator for the given vector
        /// \param num_components  number of components (i.e. phases) in the vector
        /// \param pinfo           In a parallel this holds the information about the data distribution.
        template <class Iterator>
        inline
        double euclidianNormSquared( Iterator it, const Iterator end, int num_components, const boost::any& pinfo = boost::any() )
        {
            static_cast<void>(num_components); // Suppress warning in the serial case.
            static_cast<void>(pinfo); // Suppress warning in non-MPI case.
#if HAVE_MPI
            if ( pinfo.type() == typeid(ParallelISTLInformation) )
            {
                const ParallelISTLInformation& info =
                    boost::any_cast<const ParallelISTLInformation&>(pinfo);
                int size_per_component = (end - it) / num_components;
                assert((end - it) == num_components * size_per_component);
                double component_product = 0.0;
                for( int i = 0; i < num_components; ++i )
                {
                    auto component_container =
                        boost::make_iterator_range(it + i * size_per_component,
                                                   it + (i + 1) * size_per_component);
                    info.computeReduction(component_container,
                                           Opm::Reduction::makeInnerProductFunctor<double>(),
                                           component_product);
                }
                return component_product;
            }
            else
#endif
            {
                double product = 0.0 ;
                for( ; it != end; ++it ) {
                    product += ( *it * *it );
                }
                return product;
            }
        }

        /// \brief Compute the L-infinity norm of a vector representing a well equation.
        /// \param a The container to compute the infinity norm on.
        /// \param info In a parallel this holds the information about the data distribution.
        inline
        double infinityNormWell( const ADB& a, const boost::any& pinfo )
        {
            static_cast<void>(pinfo); // Suppress warning in non-MPI case.
            double result=0;
            if( a.value().size() > 0 ) {
                result = a.value().matrix().lpNorm<Eigen::Infinity> ();
            }
#if HAVE_MPI
            if ( pinfo.type() == typeid(ParallelISTLInformation) )
            {
                const ParallelISTLInformation& real_info =
                    boost::any_cast<const ParallelISTLInformation&>(pinfo);
                result = real_info.communicator().max(result);
            }
#endif
            return result;
        }

    } // namespace detail





    template <class Grid, class Implementation>
    void BlackoilModelBase<Grid, Implementation>::updateState(const V& dx,
                                          ReservoirState& reservoir_state,
                                          WellState& well_state)
    {
        using namespace Opm::AutoDiffGrid;
        const int np = fluid_.numPhases();
        const int nc = numCells(grid_);
        const int nw = localWellsActive() ? wells().number_of_wells : 0;
        const V null;
        assert(null.size() == 0);
        const V zero = V::Zero(nc);

        // Extract parts of dx corresponding to each part.
        const V dp = subset(dx, Span(nc));
        int varstart = nc;
        const V dsw = active_[Water] ? subset(dx, Span(nc, 1, varstart)) : null;
        varstart += dsw.size();

        const V dxvar = active_[Gas] ? subset(dx, Span(nc, 1, varstart)): null;
        varstart += dxvar.size();

        // Extract well parts np phase rates + bhp
        const V dwells = subset(dx, Span((np+1)*nw, 1, varstart));
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

            const int pos = pu.phase_pos[ Oil ];
            const V so_old = s_old.col(pos);
            so = so_old - step * dso;
        }

        // Appleyard chop process.
        auto ixg = sg < 0;
        for (int c = 0; c < nc; ++c) {
            if (ixg[c]) {
                sw[c] = sw[c] / (1-sg[c]);
                so[c] = so[c] / (1-sg[c]);
                sg[c] = 0;
            }
        }


        auto ixo = so < 0;
        for (int c = 0; c < nc; ++c) {
            if (ixo[c]) {
                sw[c] = sw[c] / (1-so[c]);
                sg[c] = sg[c] / (1-so[c]);
                so[c] = 0;
            }
        }

        auto ixw = sw < 0;
        for (int c = 0; c < nc; ++c) {
            if (ixw[c]) {
                so[c] = so[c] / (1-sw[c]);
                sg[c] = sg[c] / (1-sw[c]);
                sw[c] = 0;
            }
        }

        //const V sumSat = sw + so + sg;
        //sw = sw / sumSat;
        //so = so / sumSat;
        //sg = sg / sumSat;

        // Update the reservoir_state
        for (int c = 0; c < nc; ++c) {
            reservoir_state.saturation()[c*np + pu.phase_pos[ Water ]] = sw[c];
        }

        for (int c = 0; c < nc; ++c) {
            reservoir_state.saturation()[c*np + pu.phase_pos[ Gas ]] = sg[c];
        }

        if (active_[ Oil ]) {
            const int pos = pu.phase_pos[ Oil ];
            for (int c = 0; c < nc; ++c) {
                reservoir_state.saturation()[c*np + pos] = so[c];
            }
        }

        // Update rs and rv
        const double drmaxrel = drMaxRel();
        V rs;
        if (has_disgas_) {
            const V rs_old = Eigen::Map<const V>(&reservoir_state.gasoilratio()[0], nc);
            const V drs = isRs_ * dxvar;
            const V drs_limited = sign(drs) * drs.abs().min(rs_old.abs()*drmaxrel);
            rs = rs_old - drs_limited;
        }
        V rv;
        if (has_vapoil_) {
            const V rv_old = Eigen::Map<const V>(&reservoir_state.rv()[0], nc);
            const V drv = isRv_ * dxvar;
            const V drv_limited = sign(drv) * drv.abs().min(rv_old.abs()*drmaxrel);
            rv = rv_old - drv_limited;
        }

        // Sg is used as primal variable for water only cells.
        const double epsilon = std::sqrt(std::numeric_limits<double>::epsilon());
        auto watOnly = sw >  (1 - epsilon);

        // phase translation sg <-> rs
        std::fill(primalVariable_.begin(), primalVariable_.end(), PrimalVariables::Sg);

        if (has_disgas_) {
            const V rsSat0 = fluidRsSat(p_old, s_old.col(pu.phase_pos[Oil]), cells_);
            const V rsSat = fluidRsSat(p, so, cells_);
            // The obvious case
            auto hasGas = (sg > 0 && isRs_ == 0);

            // Set oil saturated if previous rs is sufficiently large
            const V rs_old = Eigen::Map<const V>(&reservoir_state.gasoilratio()[0], nc);
            auto gasVaporized =  ( (rs > rsSat * (1+epsilon) && isRs_ == 1 ) && (rs_old > rsSat0 * (1-epsilon)) );
            auto useSg = watOnly || hasGas || gasVaporized;
            for (int c = 0; c < nc; ++c) {
                if (useSg[c]) {
                    rs[c] = rsSat[c];
                } else {
                    primalVariable_[c] = PrimalVariables::RS;
                }
            }

        }

        // phase transitions so <-> rv
        if (has_vapoil_) {

            // The gas pressure is needed for the rvSat calculations
            const V gaspress_old = computeGasPressure(p_old, s_old.col(Water), s_old.col(Oil), s_old.col(Gas));
            const V gaspress = computeGasPressure(p, sw, so, sg);
            const V rvSat0 = fluidRvSat(gaspress_old, s_old.col(pu.phase_pos[Oil]), cells_);
            const V rvSat = fluidRvSat(gaspress, so, cells_);

            // The obvious case
            auto hasOil = (so > 0 && isRv_ == 0);

            // Set oil saturated if previous rv is sufficiently large
            const V rv_old = Eigen::Map<const V>(&reservoir_state.rv()[0], nc);
            auto oilCondensed = ( (rv > rvSat * (1+epsilon) && isRv_ == 1) && (rv_old > rvSat0 * (1-epsilon)) );
            auto useSg = watOnly || hasOil || oilCondensed;
            for (int c = 0; c < nc; ++c) {
                if (useSg[c]) {
                    rv[c] = rvSat[c];
                } else {
                    primalVariable_[c] = PrimalVariables::RV;
                }
            }

        }

        // Update the reservoir_state
        if (has_disgas_) {
            std::copy(&rs[0], &rs[0] + nc, reservoir_state.gasoilratio().begin());
        }

        if (has_vapoil_) {
            std::copy(&rv[0], &rv[0] + nc, reservoir_state.rv().begin());
        }


        asImpl().updateWellState(dwells,well_state);

        // Update phase conditions used for property calculations.
        updatePhaseCondFromPrimalVariable();
    }





    template <class Grid, class Implementation>
    void
    BlackoilModelBase<Grid, Implementation>::updateWellState(const V& dwells,
                                                             WellState& well_state)
    {

        if( localWellsActive() )
        {
            const int np = wells().number_of_phases;
            const int nw = wells().number_of_wells;

            // Extract parts of dwells corresponding to each part.
            int varstart = 0;
            const V dqs = subset(dwells, Span(np*nw, 1, varstart));
            varstart += dqs.size();
            const V dbhp = subset(dwells, Span(nw, 1, varstart));
            varstart += dbhp.size();
            assert(varstart == dwells.size());
            const double dpmaxrel = dpMaxRel();


            // Qs update.
            // Since we need to update the wellrates, that are ordered by wells,
            // from dqs which are ordered by phase, the simplest is to compute
            // dwr, which is the data from dqs but ordered by wells.
            const DataBlock wwr = Eigen::Map<const DataBlock>(dqs.data(), np, nw).transpose();
            const V dwr = Eigen::Map<const V>(wwr.data(), nw*np);
            const V wr_old = Eigen::Map<const V>(&well_state.wellRates()[0], nw*np);
            const V wr = wr_old - dwr;
            std::copy(&wr[0], &wr[0] + wr.size(), well_state.wellRates().begin());

            // Bhp update.
            const V bhp_old = Eigen::Map<const V>(&well_state.bhp()[0], nw, 1);
            const V dbhp_limited = sign(dbhp) * dbhp.abs().min(bhp_old.abs()*dpmaxrel);
            const V bhp = bhp_old - dbhp_limited;
            std::copy(&bhp[0], &bhp[0] + bhp.size(), well_state.bhp().begin());
#if 0
            std::cout << " output wr in updateWellState " << std::endl;
            std::cout << wr << std::endl;
            std::cin.ignore();

            std::cout << " output bhp is updateWellState " << std::endl;
            std::cout << bhp << std::endl;
            std::cin.ignore();
#endif

            //Get gravity for THP hydrostatic correction
            const double gravity = detail::getGravity(geo_.gravity(), UgGridHelpers::dimensions(grid_));

            // Thp update
            const Opm::PhaseUsage& pu = fluid_.phaseUsage();
            //Loop over all wells
#pragma omp parallel for schedule(static)
            for (int w=0; w<nw; ++w) {
                const WellControls* wc = wells().ctrls[w];
                const int nwc = well_controls_get_num(wc);
                //Loop over all controls until we find a THP control
                //that specifies what we need...
                //Will only update THP for wells with THP control
                for (int ctrl_index=0; ctrl_index < nwc; ++ctrl_index) {
                    if (well_controls_iget_type(wc, ctrl_index) == THP) {
                        double aqua = 0.0;
                        double liquid = 0.0;
                        double vapour = 0.0;

                        if (active_[ Water ]) {
                            aqua = wr[w*np + pu.phase_pos[ Water ] ];
                        }
                        if (active_[ Oil ]) {
                            liquid = wr[w*np + pu.phase_pos[ Oil ] ];
                        }
                        if (active_[ Gas ]) {
                            vapour = wr[w*np + pu.phase_pos[ Gas ] ];
                        }

                        double alq = well_controls_iget_alq(wc, ctrl_index);
                        int table_id = well_controls_iget_vfp(wc, ctrl_index);

                        const WellType& well_type = wells().type[w];
                        if (well_type == INJECTOR) {
                            double dp = detail::computeHydrostaticCorrection(
                                    wells(), w, vfp_properties_.getInj()->getTable(table_id)->getDatumDepth(),
                                    well_perforation_densities_, gravity);

                            well_state.thp()[w] = vfp_properties_.getInj()->thp(table_id, aqua, liquid, vapour, bhp[w] + dp);
                        }
                        else if (well_type == PRODUCER) {
                            double dp = detail::computeHydrostaticCorrection(
                                    wells(), w, vfp_properties_.getProd()->getTable(table_id)->getDatumDepth(),
                                    well_perforation_densities_, gravity);

                            well_state.thp()[w] = vfp_properties_.getProd()->thp(table_id, aqua, liquid, vapour, bhp[w] + dp, alq);
                        }
                        else {
                            OPM_THROW(std::logic_error, "Expected INJECTOR or PRODUCER well");
                        }

                        //Assume only one THP control specified for each well
                        break;
                    }
                }
            }
        }
    }





    template <class Grid, class Implementation>
    std::vector<ADB>
    BlackoilModelBase<Grid, Implementation>::computeRelPerm(const SolutionState& state) const
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





    template <class Grid, class Implementation>
    std::vector<ADB>
    BlackoilModelBase<Grid, Implementation>::
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
            pressure[phaseIdx] = pressure[phaseIdx] - pressure[BlackoilPhases::Liquid];
        }

        // Since pcow = po - pw, but pcog = pg - po,
        // we have
        //   pw = po - pcow
        //   pg = po + pcgo
        // This is an unfortunate inconsistency, but a convention we must handle.
        for (int phaseIdx = 0; phaseIdx < BlackoilPhases::MaxNumPhases; ++phaseIdx) {
            if (phaseIdx == BlackoilPhases::Aqua) {
                pressure[phaseIdx] = po - pressure[phaseIdx];
            } else {
                pressure[phaseIdx] += po;
            }
        }

        return pressure;
    }





    template <class Grid, class Implementation>
    V
    BlackoilModelBase<Grid, Implementation>::computeGasPressure(const V& po,
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





    template <class Grid, class Implementation>
    void
    BlackoilModelBase<Grid, Implementation>::computeMassFlux(const int               actph ,
                                                             const V&                transi,
                                                             const ADB&              kr    ,
                                                             const ADB&              phasePressure,
                                                             const SolutionState&    state)
    {
        // Compute and store mobilities.
        const int canonicalPhaseIdx = canph_[ actph ];
        const std::vector<PhasePresence>& cond = phaseCondition();
        const ADB tr_mult = transMult(state.pressure);
        const ADB mu = fluidViscosity(canonicalPhaseIdx, phasePressure, state.temperature, state.rs, state.rv, cond);
        rq_[ actph ].mob = tr_mult * kr / mu;

        // Compute head differentials. Gravity potential is done using the face average as in eclipse and MRST.
        const ADB rho = fluidDensity(canonicalPhaseIdx, rq_[actph].b, state.rs, state.rv);
        const ADB rhoavg = ops_.caver * rho;
        rq_[ actph ].dh = ops_.ngrad * phasePressure - geo_.gravity()[2] * (rhoavg * (ops_.ngrad * geo_.z().matrix()));
        if (use_threshold_pressure_) {
            applyThresholdPressures(rq_[ actph ].dh);
        }

        // Compute phase fluxes with upwinding of formation value factor and mobility.
        const ADB& b   = rq_[ actph ].b;
        const ADB& mob = rq_[ actph ].mob;
        const ADB& dh  = rq_[ actph ].dh;
        UpwindSelector<double> upwind(grid_, ops_, dh.value());
        rq_[ actph ].mflux = upwind.select(b * mob) * (transi * dh);
    }





    template <class Grid, class Implementation>
    void
    BlackoilModelBase<Grid, Implementation>::applyThresholdPressures(ADB& dp)
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
        const V high_potential = (dp.value().abs() >= threshold_pressures_by_interior_face_).template cast<double>();

        // Create a sparse vector that nullifies the low potential elements.
        const M keep_high_potential(high_potential.matrix().asDiagonal());

        // Find the current sign for the threshold modification
        const V sign_dp = sign(dp.value());
        const V threshold_modification = sign_dp * threshold_pressures_by_interior_face_;

        // Modify potential and nullify where appropriate.
        dp = keep_high_potential * (dp - threshold_modification);
    }





    template <class Grid, class Implementation>
    std::vector<double>
    BlackoilModelBase<Grid, Implementation>::computeResidualNorms() const
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


    template <class Grid, class Implementation>
    double
    BlackoilModelBase<Grid, Implementation>::
    relativeChange(const SimulatorState& previous,
                   const SimulatorState& current ) const
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

    template <class Grid, class Implementation>
    double
    BlackoilModelBase<Grid, Implementation>::convergenceReduction(const Eigen::Array<double, Eigen::Dynamic, Eigen::Dynamic>& B,
                                                                  const Eigen::Array<double, Eigen::Dynamic, Eigen::Dynamic>& tempV,
                                                                  const Eigen::Array<double, Eigen::Dynamic, Eigen::Dynamic>& R,
                                                                  std::vector<double>& R_sum,
                                                                  std::vector<double>& maxCoeff,
                                                                  std::vector<double>& B_avg,
                                                                  std::vector<double>& maxNormWell,
                                                                  int nc,
                                                                  int nw) const
    {
        const int np = asImpl().numPhases();
        const int nm = asImpl().numMaterials();

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





    template <class Grid, class Implementation>
    bool
    BlackoilModelBase<Grid, Implementation>::getConvergence(const double dt, const int iteration)
    {
        const double tol_mb    = param_.tolerance_mb_;
        const double tol_cnv   = param_.tolerance_cnv_;
        const double tol_wells = param_.tolerance_wells_;

        const int nc = Opm::AutoDiffGrid::numCells(grid_);
        const int nw = localWellsActive() ? wells().number_of_wells : 0;
        const int np = asImpl().numPhases();
        const int nm = asImpl().numMaterials();
        assert(int(rq_.size()) == nm);

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
            const ADB& tempB = rq_[idx].b;
            B.col(idx)       = 1./tempB.value();
            R.col(idx)       = residual_.material_balance_eq[idx].value();
            tempV.col(idx)   = R.col(idx).abs()/pv;
        }

        const double pvSum = convergenceReduction(B, tempV, R,
                                                  R_sum, maxCoeff, B_avg, maxNormWell,
                                                  nc, nw);

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
        converged_Well = converged_Well && (residualWell < Opm::unit::barsa);
        const bool converged = converged_MB && converged_CNV && converged_Well;

        // Residual in Pascal can have high values and still be ok.
        const double maxWellResidualAllowed = 1000.0 * maxResidualAllowed();

        for (int idx = 0; idx < nm; ++idx) {
            if (std::isnan(mass_balance_residual[idx])
                || std::isnan(CNV[idx])
                || (idx < np && std::isnan(well_flux_residual[idx]))) {
                OPM_THROW(Opm::NumericalProblem, "NaN residual for phase " << materialName(idx));
            }
            if (mass_balance_residual[idx] > maxResidualAllowed()
                || CNV[idx] > maxResidualAllowed()
                || (idx < np && well_flux_residual[idx] > maxResidualAllowed())) {
                OPM_THROW(Opm::NumericalProblem, "Too large residual for phase " << materialName(idx));
            }
        }
        if (std::isnan(residualWell) || residualWell > maxWellResidualAllowed) {
            OPM_THROW(Opm::NumericalProblem, "NaN or too large residual for well control equation");
        }

        if ( terminal_output_ )
        {
            // Only rank 0 does print to std::cout
            if (iteration == 0) {
                std::cout << "\nIter";
                for (int idx = 0; idx < nm; ++idx) {
                    std::cout << "   MB(" << materialName(idx).substr(0, 3) << ") ";
                }
                for (int idx = 0; idx < nm; ++idx) {
                    std::cout << "    CNV(" << materialName(idx).substr(0, 1) << ") ";
                }
                for (int idx = 0; idx < np; ++idx) {
                    std::cout << "  W-FLUX(" << materialName(idx).substr(0, 1) << ")";
                }
                std::cout << '\n';
            }
            const std::streamsize oprec = std::cout.precision(3);
            const std::ios::fmtflags oflags = std::cout.setf(std::ios::scientific);
            std::cout << std::setw(4) << iteration;
            for (int idx = 0; idx < nm; ++idx) {
                std::cout << std::setw(11) << mass_balance_residual[idx];
            }
            for (int idx = 0; idx < nm; ++idx) {
                std::cout << std::setw(11) << CNV[idx];
            }
            for (int idx = 0; idx < np; ++idx) {
                std::cout << std::setw(11) << well_flux_residual[idx];
            }
            std::cout << std::endl;
            std::cout.precision(oprec);
            std::cout.flags(oflags);
        }
        return converged;
    }





    template <class Grid, class Implementation>
    bool
    BlackoilModelBase<Grid, Implementation>::getWellConvergence(const int iteration)
    {
        const double tol_wells = param_.tolerance_wells_;

        const int nc = Opm::AutoDiffGrid::numCells(grid_);
        const int nw = localWellsActive() ? wells().number_of_wells : 0;
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
            const ADB& tempB = rq_[idx].b;
            B.col(idx)       = 1./tempB.value();
            R.col(idx)       = residual_.material_balance_eq[idx].value();
            tempV.col(idx)   = R.col(idx).abs()/pv;
        }

        convergenceReduction(B, tempV, R, R_sum, maxCoeff, B_avg, maxNormWell, nc, nw);

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
        converged_Well = converged_Well && (residualWell < Opm::unit::barsa);
        const bool converged = converged_Well;

        // if one of the residuals is NaN, throw exception, so that the solver can be restarted
        for (int idx = 0; idx < np; ++idx) {
            if (std::isnan(well_flux_residual[idx])) {
                OPM_THROW(Opm::NumericalProblem, "NaN residual for phase " << materialName(idx));
            }
            if (well_flux_residual[idx] > maxResidualAllowed()) {
                OPM_THROW(Opm::NumericalProblem, "Too large residual for phase " << materialName(idx));
            }
        }

        if ( terminal_output_ )
        {
            // Only rank 0 does print to std::cout
            if (iteration == 0) {
                std::cout << "\nIter";
                for (int idx = 0; idx < np; ++idx) {
                    std::cout << "  W-FLUX(" << materialName(idx).substr(0, 1) << ")";
                }
                std::cout << '\n';
            }
            const std::streamsize oprec = std::cout.precision(3);
            const std::ios::fmtflags oflags = std::cout.setf(std::ios::scientific);
            std::cout << std::setw(4) << iteration;
            for (int idx = 0; idx < np; ++idx) {
                std::cout << std::setw(11) << well_flux_residual[idx];
            }
            std::cout << std::endl;
            std::cout.precision(oprec);
            std::cout.flags(oflags);
        }
        return converged;
    }





    template <class Grid, class Implementation>
    ADB
    BlackoilModelBase<Grid, Implementation>::fluidViscosity(const int               phase,
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





    template <class Grid, class Implementation>
    ADB
    BlackoilModelBase<Grid, Implementation>::fluidReciprocFVF(const int               phase,
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





    template <class Grid, class Implementation>
    ADB
    BlackoilModelBase<Grid, Implementation>::fluidDensity(const int  phase,
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





    template <class Grid, class Implementation>
    V
    BlackoilModelBase<Grid, Implementation>::fluidRsSat(const V&                p,
                                               const V&                satOil,
                                               const std::vector<int>& cells) const
    {
        return fluid_.rsSat(ADB::constant(p), ADB::constant(satOil), cells).value();
    }





    template <class Grid, class Implementation>
    ADB
    BlackoilModelBase<Grid, Implementation>::fluidRsSat(const ADB&              p,
                                               const ADB&              satOil,
                                               const std::vector<int>& cells) const
    {
        return fluid_.rsSat(p, satOil, cells);
    }





    template <class Grid, class Implementation>
    V
    BlackoilModelBase<Grid, Implementation>::fluidRvSat(const V&                p,
                                               const V&              satOil,
                                               const std::vector<int>& cells) const
    {
        return fluid_.rvSat(ADB::constant(p), ADB::constant(satOil), cells).value();
    }





    template <class Grid, class Implementation>
    ADB
    BlackoilModelBase<Grid, Implementation>::fluidRvSat(const ADB&              p,
                                               const ADB&              satOil,
                                               const std::vector<int>& cells) const
    {
        return fluid_.rvSat(p, satOil, cells);
    }





    template <class Grid, class Implementation>
    ADB
    BlackoilModelBase<Grid, Implementation>::poroMult(const ADB& p) const
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





    template <class Grid, class Implementation>
    ADB
    BlackoilModelBase<Grid, Implementation>::transMult(const ADB& p) const
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





    template <class Grid, class Implementation>
    void
    BlackoilModelBase<Grid, Implementation>::classifyCondition(const ReservoirState& state)
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





    template <class Grid, class Implementation>
    void
    BlackoilModelBase<Grid, Implementation>::updatePrimalVariableFromState(const ReservoirState& state)
    {
        using namespace Opm::AutoDiffGrid;
        const int nc = numCells(grid_);
        const int np = state.numPhases();

        const PhaseUsage& pu = fluid_.phaseUsage();
        const DataBlock s = Eigen::Map<const DataBlock>(& state.saturation()[0], nc, np);

        // Water/Oil/Gas system
        assert (active_[ Gas ]);

        // reset the primary variables if RV and RS is not set Sg is used as primary variable.
        primalVariable_.resize(nc);
        std::fill(primalVariable_.begin(), primalVariable_.end(), PrimalVariables::Sg);

        const V sg = s.col(pu.phase_pos[ Gas ]);
        const V so = s.col(pu.phase_pos[ Oil ]);
        const V sw = s.col(pu.phase_pos[ Water ]);

        const double epsilon = std::sqrt(std::numeric_limits<double>::epsilon());
        auto watOnly = sw >  (1 - epsilon);
        auto hasOil = so > 0;
        auto hasGas = sg > 0;

        // For oil only cells Rs is used as primal variable. For cells almost full of water
        // the default primal variable (Sg) is used.
        if (has_disgas_) {
            for (V::Index c = 0, e = sg.size(); c != e; ++c) {
                if ( !watOnly[c] && hasOil[c] && !hasGas[c] ) {primalVariable_[c] = PrimalVariables::RS; }
            }
        }

        // For gas only cells Rv is used as primal variable. For cells almost full of water
        // the default primal variable (Sg) is used.
        if (has_vapoil_) {
            for (V::Index c = 0, e = so.size(); c != e; ++c) {
                if ( !watOnly[c] && hasGas[c] && !hasOil[c] ) {primalVariable_[c] = PrimalVariables::RV; }
            }
        }
        updatePhaseCondFromPrimalVariable();
    }





    /// Update the phaseCondition_ member based on the primalVariable_ member.
    template <class Grid, class Implementation>
    void
    BlackoilModelBase<Grid, Implementation>::updatePhaseCondFromPrimalVariable()
    {
        if (! active_[Gas]) {
            OPM_THROW(std::logic_error, "updatePhaseCondFromPrimarVariable() logic requires active gas phase.");
        }
        const int nc = primalVariable_.size();
        isRs_ = V::Zero(nc);
        isRv_ = V::Zero(nc);
        isSg_ = V::Zero(nc);
        for (int c = 0; c < nc; ++c) {
            phaseCondition_[c] = PhasePresence(); // No free phases.
            phaseCondition_[c].setFreeWater(); // Not necessary for property calculation usage.
            switch (primalVariable_[c]) {
            case PrimalVariables::Sg:
                phaseCondition_[c].setFreeOil();
                phaseCondition_[c].setFreeGas();
                isSg_[c] = 1;
                break;
            case PrimalVariables::RS:
                phaseCondition_[c].setFreeOil();
                isRs_[c] = 1;
                break;
            case PrimalVariables::RV:
                phaseCondition_[c].setFreeGas();
                isRv_[c] = 1;
                break;
            default:
                OPM_THROW(std::logic_error, "Unknown primary variable enum value in cell " << c << ": " << primalVariable_[c]);
            }
        }
    }




} // namespace Opm

#endif // OPM_BLACKOILMODELBASE_IMPL_HEADER_INCLUDED
