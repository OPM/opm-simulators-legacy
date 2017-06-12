/*
  Copyright 2015, 2016 SINTEF ICT, Applied Mathematics.
  Copyright 2016 Statoil AS.

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

#ifndef OPM_BLACKOILTRANSPORTMODEL_HEADER_INCLUDED
#define OPM_BLACKOILTRANSPORTMODEL_HEADER_INCLUDED

#include <opm/autodiff/BlackoilModelBase.hpp>
#include <opm/core/simulator/BlackoilState.hpp>
#include <opm/autodiff/WellStateFullyImplicitBlackoil.hpp>
#include <opm/autodiff/BlackoilModelParameters.hpp>
#include <opm/simulators/timestepping/SimulatorTimerInterface.hpp>
#include <opm/autodiff/multiPhaseUpwind.hpp>

namespace Opm {

    /// A model implementation for the transport equation in three-phase black oil.
    template<class Grid, class WellModel>
    class BlackoilTransportModel : public BlackoilModelBase<Grid, WellModel, BlackoilTransportModel<Grid, WellModel> >
    {
    public:
        typedef BlackoilModelBase<Grid, WellModel, BlackoilTransportModel<Grid, WellModel> > Base;
        friend Base;

        typedef typename Base::ReservoirState ReservoirState;
        typedef typename Base::WellState WellState;
        typedef typename Base::SolutionState SolutionState;
        typedef typename Base::V V;


        /// Construct the model. It will retain references to the
        /// arguments of this functions, and they are expected to
        /// remain in scope for the lifetime of the solver.
        /// \param[in] param            parameters
        /// \param[in] grid             grid data structure
        /// \param[in] fluid            fluid properties
        /// \param[in] geo              rock properties
        /// \param[in] rock_comp_props  if non-null, rock compressibility properties
        /// \param[in] wells_arg        well structure
        /// \param[in] linsolver        linear solver
        /// \param[in] eclState         eclipse state
        /// \param[in] has_disgas       turn on dissolved gas
        /// \param[in] has_vapoil       turn on vaporized oil feature
        /// \param[in] terminal_output  request output to cout/cerr
        BlackoilTransportModel(const typename Base::ModelParameters&   param,
                               const Grid&                             grid,
                               const BlackoilPropsAdFromDeck&         fluid,
                               const DerivedGeology&                   geo,
                               const RockCompressibility*              rock_comp_props,
                               const StandardWells&                    std_wells,
                               const NewtonIterationBlackoilInterface& linsolver,
                               std::shared_ptr<const EclipseState>     eclState,
                               const bool                              has_disgas,
                               const bool                              has_vapoil,
                               const bool                              terminal_output)
            : Base(param, grid, fluid, geo, rock_comp_props, std_wells, linsolver,
                   eclState, has_disgas, has_vapoil, terminal_output)
        {
        }

        void prepareStep(const SimulatorTimerInterface& timer,
                         const ReservoirState& reservoir_state,
                         const WellState& well_state)
        {
            Base::prepareStep(timer, reservoir_state, well_state);
            Base::param_.solve_welleq_initially_ = false;
            state0_ = variableState(reservoir_state, well_state);
            asImpl().makeConstantState(state0_);
        }

        SimulatorReport
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

            // Create the primary variables.
            SolutionState state = asImpl().variableState(reservoir_state, well_state);

            if (initial_assembly) {

                // HACK
                const_cast<V&>(total_flux_)
                    = Eigen::Map<const V>(reservoir_state.faceflux().data(), reservoir_state.faceflux().size());
                const_cast<V&>(total_wellperf_flux_)
                    = Eigen::Map<const V>(well_state.perfRates().data(), well_state.perfRates().size());
                const_cast<DataBlock&>(comp_wellperf_flux_)
                    = Eigen::Map<const DataBlock>(well_state.perfPhaseRates().data(), well_state.perfRates().size(), numPhases());
                assert(numPhases() * well_state.perfRates().size() == well_state.perfPhaseRates().size());

                is_first_iter_ = true;
                // Create the (constant, derivativeless) initial state.
                SolutionState state0 = state;
                asImpl().makeConstantState(state0);
                // Compute initial accumulation contributions
                // and well connection pressures.
                asImpl().computeAccum(state0, 0);
                asImpl().wellModel().computeWellConnectionPressures(state0, well_state);
            } else {
                is_first_iter_ = false;
            }

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

            // @afr changed
            // asImpl().wellModel().computeWellFlux(state, mob_perfcells, b_perfcells, aliveWells, cq_s);
            asImpl().computeWellFlux(state, mob_perfcells, b_perfcells, aliveWells, cq_s);
            // end of changed
            asImpl().wellModel().updatePerfPhaseRatesAndPressures(cq_s, state, well_state);
            asImpl().wellModel().addWellFluxEq(cq_s, state, residual_);
            asImpl().addWellContributionToMassBalanceEq(cq_s, state, well_state);
            asImpl().wellModel().addWellControlEq(state, well_state, aliveWells, residual_);

            return report;
        }




        /// Solve the Jacobian system Jx = r where J is the Jacobian and
        /// r is the residual.
        V solveJacobianSystem() const
        {
            const int n_transport = residual_.material_balance_eq[1].size();
            const int n_full = residual_.sizeNonLinear();
            const auto& mb = residual_.material_balance_eq;
            LinearisedBlackoilResidual transport_res = {
                {
                    // TODO: handle general 2-phase etc.
                    ADB::function(mb[1].value(), { mb[1].derivative()[1], mb[1].derivative()[2] }),
                    ADB::function(mb[2].value(), { mb[2].derivative()[1], mb[2].derivative()[2] })
                },
                ADB::null(),
                ADB::null(),
                residual_.matbalscale,
                residual_.singlePrecision
            };
            assert(transport_res.sizeNonLinear() == 2*n_transport);
            V dx_transport = linsolver_.computeNewtonIncrement(transport_res);
            assert(dx_transport.size() == 2*n_transport);
            V dx_full = V::Zero(n_full);
            for (int i = 0; i < 2*n_transport; ++i) {
                dx_full(n_transport + i) = dx_transport(i);
            }
            return dx_full;
        }





        using Base::numPhases;
        using Base::numMaterials;

    protected:
        using Base::asImpl;
        using Base::materialName;
        using Base::convergenceReduction;
        using Base::maxResidualAllowed;

        using Base::linsolver_;
        using Base::residual_;
        using Base::sd_;
        using Base::geo_;
        using Base::ops_;
        using Base::grid_;
        using Base::use_threshold_pressure_;
        using Base::canph_;
        using Base::active_;
        using Base::pvdt_;
        using Base::fluid_;
        using Base::param_;
        using Base::terminal_output_;

        using Base::isVFPActive;
        using Base::phaseCondition;
        using Base::vfp_properties_;
        using Base::wellsActive;

        V total_flux_; // HACK, should be part of a revised (transport-specific) SolutionState.
        V total_wellperf_flux_;
        DataBlock comp_wellperf_flux_;
        SolutionState state0_ = SolutionState(3);
        bool is_first_iter_ = false;
        Eigen::Array<double, Eigen::Dynamic, Eigen::Dynamic> upwind_flags_;


        SolutionState
        variableState(const ReservoirState& x,
                      const WellState& xw) const
        {
            // As Base::variableState(), except making Pressure, Qs and Bhp constants.
            std::vector<V> vars0 = asImpl().variableStateInitials(x, xw);
            std::vector<ADB> vars = ADB::variables(vars0);
            const std::vector<int> indices = asImpl().variableStateIndices();
            vars[indices[Pressure]] = ADB::constant(vars[indices[Pressure]].value());
            vars[indices[Qs]] = ADB::constant(vars[indices[Qs]].value());
            vars[indices[Bhp]] = ADB::constant(vars[indices[Bhp]].value());
            return asImpl().variableStateExtractVars(x, indices, vars);
        }





        void computeAccum(const SolutionState& state,
                          const int            aix  )
        {
            if (aix == 0) {
                // The pressure passed in state is from after
                // the pressure solver, but we need to use the original
                // b factors etc. to get the initial accumulation term
                // correct.
                Base::computeAccum(state0_, aix);
            } else {
                Base::computeAccum(state, aix);
            }
        }









        void assembleMassBalanceEq(const SolutionState& state)
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
            const ADB tr_mult = asImpl().transMult(state.pressure);
            const V gdz = geo_.gravity()[2] * (ops_.grad * geo_.z().matrix());

            if (is_first_iter_) {
                upwind_flags_.resize(gdz.size(), numPhases());
            }

            // Compute mobilities and heads
            const std::vector<PhasePresence>& cond = asImpl().phaseCondition();
            const std::vector<ADB> kr = asImpl().computeRelPerm(state);
#pragma omp parallel for schedule(static)
            for (int phase_idx = 0; phase_idx < numPhases(); ++phase_idx) {
                // Compute and store mobilities.
                const int canonical_phase_idx = canph_[ phase_idx ];
                const ADB& phase_pressure = state.canonical_phase_pressures[canonical_phase_idx];
                sd_.rq[phase_idx].mu = asImpl().fluidViscosity(canonical_phase_idx, phase_pressure, state.temperature, state.rs, state.rv, cond);
                sd_.rq[phase_idx].kr = kr[canonical_phase_idx];
                // Note that the pressure-dependent transmissibility multipliers are considered
                // part of the mobility here.
                sd_.rq[ phase_idx ].mob = tr_mult * sd_.rq[phase_idx].kr / sd_.rq[phase_idx].mu;

                // Compute head differentials. Gravity potential is done using the face average as in eclipse and MRST.
                sd_.rq[phase_idx].rho = asImpl().fluidDensity(canonical_phase_idx, sd_.rq[phase_idx].b, state.rs, state.rv);
                const ADB rhoavg = ops_.caver * sd_.rq[phase_idx].rho;
                sd_.rq[ phase_idx ].dh = ops_.grad * phase_pressure -  rhoavg * gdz;

                if (is_first_iter_) {
                    upwind_flags_.col(phase_idx) = -sd_.rq[phase_idx].dh.value();
                }

                if (use_threshold_pressure_) {
                    asImpl().applyThresholdPressures(sd_.rq[ phase_idx ].dh);
                }
            }

            // Extract saturation-dependent part of head differences.
            const ADB gradp = ops_.grad * state.pressure;
            std::vector<ADB> dh_sat(numPhases(), ADB::null());
            for (int phase_idx = 0; phase_idx < numPhases(); ++phase_idx) {
                dh_sat[phase_idx] = gradp - sd_.rq[phase_idx].dh;
            }

            // Find upstream directions for each phase.
            upwind_flags_ = multiPhaseUpwind(dh_sat, trans_all);

            // Compute (upstream) phase and total mobilities for connections.
            // Also get upstream b, rs, and rv values to avoid recreating the UpwindSelector.
            std::vector<ADB> mob(numPhases(), ADB::null());
            std::vector<ADB> b(numPhases(), ADB::null());
            ADB rs = ADB::null();
            ADB rv = ADB::null();
            ADB tot_mob = ADB::constant(V::Zero(gdz.size()));
            for (int phase_idx = 0; phase_idx < numPhases(); ++phase_idx) {
                UpwindSelector<double> upwind(grid_, ops_, upwind_flags_.col(phase_idx));
                mob[phase_idx] = upwind.select(sd_.rq[phase_idx].mob);
                tot_mob += mob[phase_idx];
                b[phase_idx] = upwind.select(sd_.rq[phase_idx].b);
                if (canph_[phase_idx] == Oil) {
                    rs = upwind.select(state.rs);
                }
                if (canph_[phase_idx] == Gas) {
                    rv = upwind.select(state.rv);
                }
            }

            // Compute phase fluxes.
            for (int phase_idx = 0; phase_idx < numPhases(); ++phase_idx) {
                ADB gflux = ADB::constant(V::Zero(gdz.size()));
                for (int other_phase = 0; other_phase < numPhases(); ++other_phase) {
                    if (phase_idx != other_phase) {
                        gflux += mob[other_phase] * (dh_sat[phase_idx] - dh_sat[other_phase]);
                    }
                }
                sd_.rq[phase_idx].mflux = b[phase_idx] * (mob[phase_idx] / tot_mob) * (total_flux_ + trans_all * gflux);
            }

#pragma omp parallel for schedule(static)
            for (int phase_idx = 0; phase_idx < numPhases(); ++phase_idx) {
                // const int canonical_phase_idx = canph_[ phase_idx ];
                // const ADB& phase_pressure = state.canonical_phase_pressures[canonical_phase_idx];
                // asImpl().computeMassFlux(phase_idx, trans_all, kr[canonical_phase_idx], phase_pressure, state);

                // Material balance equation for this phase.
                residual_.material_balance_eq[ phase_idx ] =
                    pvdt_ * (sd_.rq[phase_idx].accum[1] - sd_.rq[phase_idx].accum[0])
                    + ops_.div*sd_.rq[phase_idx].mflux;
            }

            // -------- Extra (optional) rs and rv contributions to the mass balance equations --------

            // Add the extra (flux) terms to the mass balance equations
            // From gas dissolved in the oil phase (rs) and oil vaporized in the gas phase (rv)
            // The extra terms in the accumulation part of the equation are already handled.
            if (active_[ Oil ] && active_[ Gas ]) {
                const int po = fluid_.phaseUsage().phase_pos[ Oil ];
                const int pg = fluid_.phaseUsage().phase_pos[ Gas ];
                residual_.material_balance_eq[ pg ] += ops_.div * (rs * sd_.rq[po].mflux);
                residual_.material_balance_eq[ po ] += ops_.div * (rv * sd_.rq[pg].mflux);
            }

            if (param_.update_equations_scaling_) {
                asImpl().updateEquationsScaling();
            }

        }





        Eigen::Array<double, Eigen::Dynamic, Eigen::Dynamic> multiPhaseUpwind(const std::vector<ADB>& head_diff,
                                                                              const V& transmissibility)
        {
            assert(numPhases() == 3);
            const int num_connections = head_diff[0].size();
            Eigen::Array<double, Eigen::Dynamic, Eigen::Dynamic> upwind(num_connections, numPhases());
            for (int conn = 0; conn < num_connections; ++conn) {
                const double q = total_flux_[conn];
                const double t = transmissibility[conn];
                const int a = ops_.connection_cells(conn, 0); // first cell of connection
                const int b = ops_.connection_cells(conn, 1); // second cell of connection
                auto up = connectionMultiPhaseUpwind(
                    {{ head_diff[0].value()[conn], head_diff[1].value()[conn], head_diff[2].value()[conn] }},
                    {{ sd_.rq[0].mob.value()[a], sd_.rq[1].mob.value()[a], sd_.rq[2].mob.value()[a]}},
                    {{ sd_.rq[0].mob.value()[b], sd_.rq[1].mob.value()[b], sd_.rq[2].mob.value()[b]}},
                    t,
                    q);
                for (int ii = 0; ii < numPhases(); ++ii) {
                    upwind(conn, ii) = up[ii];
                }
            }
            return upwind;
        }





        void computeWellFlux(const SolutionState& state,
                             const std::vector<ADB>& mob_perfcells,
                             const std::vector<ADB>& b_perfcells,
                             V& /* aliveWells */,
                             std::vector<ADB>& cq_s) const
        {
            // Note that use of this function replaces using the well models'
            // function of the same name.
            if( ! asImpl().localWellsActive() ) return ;

            const int np = asImpl().wells().number_of_phases;
            const int nw = asImpl().wells().number_of_wells;
            const int nperf = asImpl().wells().well_connpos[nw];
            const Opm::PhaseUsage& pu = asImpl().fluid_.phaseUsage();

            // Compute total mobilities for perforations.
            ADB totmob_perfcells = ADB::constant(V::Zero(nperf));
            for (int phase = 0; phase < numPhases(); ++phase) {
                totmob_perfcells += mob_perfcells[phase];
            }

            // Compute fractional flow.
            std::vector<ADB> frac_flow(np, ADB::null());
            for (int phase = 0; phase < np; ++phase) {
                frac_flow[phase] = mob_perfcells[phase] / totmob_perfcells;
            }

            // Identify injecting and producing perforations.
            V is_inj = V::Zero(nperf);
            V is_prod = V::Zero(nperf);
            for (int c = 0; c < nperf; ++c){
                if (total_wellperf_flux_[c] > 0.0) {
                    is_inj[c] = 1;
                } else {
                    is_prod[c] = 1;
                }
            }

            // Compute fluxes for producing perforations.
            std::vector<ADB> cq_s_prod(3, ADB::null());
            for (int phase = 0; phase < np; ++phase) {
                // For producers, we use the total reservoir flux from the pressure solver.
                cq_s_prod[phase] = b_perfcells[phase] * frac_flow[phase] * total_wellperf_flux_;
            }
            if (asImpl().has_disgas_ || asImpl().has_vapoil_) {
                const int oilpos = pu.phase_pos[Oil];
                const int gaspos = pu.phase_pos[Gas];
                const ADB cq_s_prod_oil = cq_s_prod[oilpos];
                const ADB cq_s_prod_gas = cq_s_prod[gaspos];
                cq_s_prod[gaspos] += subset(state.rs, Base::well_model_.wellOps().well_cells) * cq_s_prod_oil;
                cq_s_prod[oilpos] += subset(state.rv, Base::well_model_.wellOps().well_cells) * cq_s_prod_gas;
            }

            // Compute well perforation surface volume fluxes.
            cq_s.resize(np, ADB::null());
            for (int phase = 0; phase < np; ++phase) {
                const int pos = pu.phase_pos[phase];
                // For injectors, we use the component fluxes computed by the pressure solver.
                const V cq_s_inj = comp_wellperf_flux_.col(pos);
                cq_s[phase] = is_prod * cq_s_prod[phase] + is_inj * cq_s_inj;
            }
        }





        bool getConvergence(const SimulatorTimerInterface& timer, const int iteration)
        {
            const double dt = timer.currentStepLength();
            const double tol_mb    = param_.tolerance_mb_;
            const double tol_cnv   = param_.tolerance_cnv_;

            const int nc = Opm::AutoDiffGrid::numCells(grid_);
            const int np = asImpl().numPhases();
            const int nm = asImpl().numMaterials();
            assert(int(sd_.rq.size()) == nm);

            const V& pv = geo_.poreVolume();

            std::vector<double> R_sum(nm);
            std::vector<double> B_avg(nm);
            std::vector<double> maxCoeff(nm);
            std::vector<double> maxNormWell(np);
            Eigen::Array<typename V::Scalar, Eigen::Dynamic, Eigen::Dynamic> B(nc, nm);
            Eigen::Array<typename V::Scalar, Eigen::Dynamic, Eigen::Dynamic> R(nc, nm);
            Eigen::Array<typename V::Scalar, Eigen::Dynamic, Eigen::Dynamic> tempV(nc, nm);

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
            // Finish computation
            for ( int idx = 1; idx < nm; ++idx ) {
                CNV[idx]                    = B_avg[idx] * dt * maxCoeff[idx];
                mass_balance_residual[idx]  = std::abs(B_avg[idx]*R_sum[idx]) * dt / pvSum;
                converged_MB                = converged_MB && (mass_balance_residual[idx] < tol_mb);
                converged_CNV               = converged_CNV && (CNV[idx] < tol_cnv);
                assert(nm >= np);
            }

            const bool converged = converged_MB && converged_CNV;

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

            if ( terminal_output_ ) {
                // Only rank 0 does print to std::cout
                std::ostringstream os;
                if (iteration == 0) {
                    os << "\nIter";
                    for (int idx = 1; idx < nm; ++idx) {
                        os << "   MB(" << materialName(idx).substr(0, 3) << ") ";
                    }
                    for (int idx = 1; idx < nm; ++idx) {
                        os << "    CNV(" << materialName(idx).substr(0, 1) << ") ";
                    }
                    os << '\n';
                }
                os.precision(3);
                os.setf(std::ios::scientific);
                os << std::setw(4) << iteration;
                for (int idx = 1; idx < nm; ++idx) {
                    os << std::setw(11) << mass_balance_residual[idx];
                }
                for (int idx = 1; idx < nm; ++idx) {
                    os << std::setw(11) << CNV[idx];
                }
                OpmLog::info(os.str());
            }
            return converged;
        }
    };


    /// Providing types by template specialisation of ModelTraits for BlackoilTransportModel.
    template <class Grid, class WellModel>
    struct ModelTraits< BlackoilTransportModel<Grid, WellModel> >
    {
        typedef BlackoilState ReservoirState;
        typedef WellStateFullyImplicitBlackoil WellState;
        typedef BlackoilModelParameters ModelParameters;
        typedef DefaultBlackoilSolutionState SolutionState;
    };

} // namespace Opm




#endif // OPM_BLACKOILTRANSPORTMODEL_HEADER_INCLUDED
