/*
  Copyright 2013, 2015 SINTEF ICT, Applied Mathematics.
  Copyright 2014, 2015 Statoil ASA.
  Copyright 2014, 2015 Dr. Markus Blatt - HPC-Simulation-Software & Services
  Copyright 2015 NTNU

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

#ifndef OPM_BLACKOILMODELBASE_HEADER_INCLUDED
#define OPM_BLACKOILMODELBASE_HEADER_INCLUDED

#include <cassert>

#include <opm/autodiff/AutoDiffBlock.hpp>
#include <opm/autodiff/AutoDiffHelpers.hpp>
#include <opm/autodiff/BlackoilPropsAdInterface.hpp>
#include <opm/autodiff/LinearisedBlackoilResidual.hpp>
#include <opm/autodiff/NewtonIterationBlackoilInterface.hpp>
#include <opm/autodiff/BlackoilModelEnums.hpp>
#include <opm/autodiff/VFPProperties.hpp>
#include <opm/autodiff/RateConverter.hpp>
#include <opm/parser/eclipse/EclipseState/Grid/NNC.hpp>
#include <opm/core/simulator/SimulatorTimerInterface.hpp>

#include <array>

struct Wells;

namespace Opm {

    namespace parameter { class ParameterGroup; }
    class DerivedGeology;
    class RockCompressibility;
    class NewtonIterationBlackoilInterface;
    class VFPProperties;
    class SimulationDataContainer;

    /// Struct for containing iteration variables.
    struct DefaultBlackoilSolutionState
    {
        typedef AutoDiffBlock<double> ADB;
        explicit DefaultBlackoilSolutionState(const int np)
            : pressure  (    ADB::null())
            , temperature(   ADB::null())
            , saturation(np, ADB::null())
            , rs        (    ADB::null())
            , rv        (    ADB::null())
            , qs        (    ADB::null())
            , bhp       (    ADB::null())
            , canonical_phase_pressures(3, ADB::null())
        {
        }
        ADB              pressure;
        ADB              temperature;
        std::vector<ADB> saturation;
        ADB              rs;
        ADB              rv;
        ADB              qs;
        ADB              bhp;
        // Below are quantities stored in the state for optimization purposes.
        std::vector<ADB> canonical_phase_pressures; // Always has 3 elements, even if only 2 phases active.
    };




    /// Class used for reporting the outcome of a nonlinearIteration() call.
    struct IterationReport
    {
        bool failed;
        bool converged;
        int linear_iterations;
        int well_iterations;
    };




    /// Traits to encapsulate the types used by classes using or
    /// extending this model. Forward declared here, must be
    /// specialised for each concrete model class.
    template <class ConcreteModel>
    struct ModelTraits;


    /// A model implementation for three-phase black oil.
    ///
    /// The simulator is capable of handling three-phase problems
    /// where gas can be dissolved in oil and vice versa. It
    /// uses an industry-standard TPFA discretization with per-phase
    /// upwind weighting of mobilities.
    ///
    /// It uses automatic differentiation via the class AutoDiffBlock
    /// to simplify assembly of the jacobian matrix.
    /// \tparam  Grid            UnstructuredGrid or CpGrid.
    /// \tparam  WellModel       WellModel employed.
    /// \tparam  Implementation  Provides concrete state types.
    template<class Grid, class WellModel, class Implementation>
    class BlackoilModelBase
    {
    public:
        // ---------  Types and enums  ---------
        typedef AutoDiffBlock<double> ADB;
        typedef ADB::V V;
        typedef ADB::M M;

        struct ReservoirResidualQuant {
            ReservoirResidualQuant();
            std::vector<ADB> accum; // Accumulations
            ADB              mflux; // Mass flux (surface conditions)
            ADB              b;     // Reciprocal FVF
            ADB              mu;    // Viscosities
            ADB              rho;   // Densities
            ADB              kr;    // Permeabilities
            ADB              dh;    // Pressure drop across int. interfaces
            ADB              mob;   // Phase mobility (per cell)
        };

        struct SimulatorData {
            SimulatorData(int num_phases);

            enum FipId {
                FIP_AQUA = Opm::Water,
                FIP_LIQUID = Opm::Oil,
                FIP_VAPOUR = Opm::Gas,
                FIP_DISSOLVED_GAS = 3,
                FIP_VAPORIZED_OIL = 4,
                FIP_PV = 5,                    //< Pore volume
                FIP_WEIGHTED_PRESSURE = 6
            };
            std::vector<ReservoirResidualQuant> rq;
            ADB rsSat; // Saturated gas-oil ratio
            ADB rvSat; // Saturated oil-gas ratio
            std::array<V, 7> fip;
        };

        typedef typename ModelTraits<Implementation>::ReservoirState ReservoirState;
        typedef typename ModelTraits<Implementation>::WellState WellState;
        typedef typename ModelTraits<Implementation>::ModelParameters ModelParameters;
        typedef typename ModelTraits<Implementation>::SolutionState SolutionState;

        // for the conversion between the surface volume rate and resrevoir voidage rate
        // Due to the requirement of the grid information, put the converter in the model.
        using RateConverterType = RateConverter::
                                  SurfaceToReservoirVoidage<BlackoilPropsAdInterface, std::vector<int> >;

        // ---------  Public methods  ---------

        /// Construct the model. It will retain references to the
        /// arguments of this functions, and they are expected to
        /// remain in scope for the lifetime of the solver.
        /// \param[in] param            parameters
        /// \param[in] grid             grid data structure
        /// \param[in] fluid            fluid properties
        /// \param[in] geo              rock properties
        /// \param[in] rock_comp_props  if non-null, rock compressibility properties
        /// \param[in] wells            well structure
        /// \param[in] vfp_properties   Vertical flow performance tables
        /// \param[in] linsolver        linear solver
        /// \param[in] eclState         eclipse state
        /// \param[in] has_disgas       turn on dissolved gas
        /// \param[in] has_vapoil       turn on vaporized oil feature
        /// \param[in] terminal_output  request output to cout/cerr
        BlackoilModelBase(const ModelParameters&          param,
                          const Grid&                     grid ,
                          const BlackoilPropsAdInterface& fluid,
                          const DerivedGeology&           geo  ,
                          const RockCompressibility*      rock_comp_props,
                          const WellModel&                well_model,
                          const NewtonIterationBlackoilInterface& linsolver,
                          std::shared_ptr< const EclipseState > eclState,
                          const bool has_disgas,
                          const bool has_vapoil,
                          const bool terminal_output);

        /// \brief Set threshold pressures that prevent or reduce flow.
        /// This prevents flow across faces if the potential
        /// difference is less than the threshold. If the potential
        /// difference is greater, the threshold value is subtracted
        /// before calculating flow. This is treated symmetrically, so
        /// flow is prevented or reduced in both directions equally.
        /// \param[in]  threshold_pressures_by_face   array of size equal to the number of faces
        ///                                   of the grid passed in the constructor.
        void setThresholdPressures(const std::vector<double>& threshold_pressures_by_face);

        /// Called once before each time step.
        /// \param[in] timer                  simulation timer
        /// \param[in, out] reservoir_state   reservoir state variables
        /// \param[in, out] well_state        well state variables
        void prepareStep(const SimulatorTimerInterface& timer,
                         const ReservoirState& reservoir_state,
                         const WellState& well_state);

        /// Called once per nonlinear iteration.
        /// This model will perform a Newton-Raphson update, changing reservoir_state
        /// and well_state. It will also use the nonlinear_solver to do relaxation of
        /// updates if necessary.
        /// \param[in] iteration              should be 0 for the first call of a new timestep
        /// \param[in] timer                  simulation timer
        /// \param[in] nonlinear_solver       nonlinear solver used (for oscillation/relaxation control)
        /// \param[in, out] reservoir_state   reservoir state variables
        /// \param[in, out] well_state        well state variables
        template <class NonlinearSolverType>
        IterationReport nonlinearIteration(const int iteration,
                                           const SimulatorTimerInterface& timer,
                                           NonlinearSolverType& nonlinear_solver,
                                           ReservoirState& reservoir_state,
                                           WellState& well_state);

        /// Called once after each time step.
        /// In this class, this function does nothing.
        /// \param[in] timer                  simulation timer
        /// \param[in, out] reservoir_state   reservoir state variables
        /// \param[in, out] well_state        well state variables
        void afterStep(const SimulatorTimerInterface& timer,
                       ReservoirState& reservoir_state,
                       WellState& well_state);

        /// Assemble the residual and Jacobian of the nonlinear system.
        /// \param[in]      reservoir_state   reservoir state variables
        /// \param[in, out] well_state        well state variables
        /// \param[in]      initial_assembly  pass true if this is the first call to assemble() in this timestep
        /// \return well iterations.
        IterationReport
        assemble(const ReservoirState& reservoir_state,
                 WellState& well_state,
                 const bool initial_assembly);

        /// \brief Compute the residual norms of the mass balance for each phase,
        /// the well flux, and the well equation.
        /// \return a vector that contains for each phase the norm of the mass balance
        /// and afterwards the norm of the residual of the well flux and the well equation.
        std::vector<double> computeResidualNorms() const;

        /// \brief compute the relative change between to simulation states
        //  \return || u^n+1 - u^n || / || u^n+1 ||
        double relativeChange( const SimulationDataContainer& previous, const SimulationDataContainer& current ) const;

        /// The size (number of unknowns) of the nonlinear system of equations.
        int sizeNonLinear() const;

        /// Number of linear iterations used in last call to solveJacobianSystem().
        int linearIterationsLastSolve() const;

        /// Solve the Jacobian system Jx = r where J is the Jacobian and
        /// r is the residual.
        V solveJacobianSystem() const;

        /// Apply an update to the primary variables, chopped if appropriate.
        /// \param[in]      dx                updates to apply to primary variables
        /// \param[in, out] reservoir_state   reservoir state variables
        /// \param[in, out] well_state        well state variables
        void updateState(const V& dx,
                         ReservoirState& reservoir_state,
                         WellState& well_state);

        /// Return true if this is a parallel run.
        bool isParallel() const;

        /// Return true if output to cout is wanted.
        bool terminalOutputEnabled() const;

        /// Compute convergence based on total mass balance (tol_mb) and maximum
        /// residual mass balance (tol_cnv).
        /// \param[in]   timer       simulation timer
        /// \param[in]   iteration   current iteration number
        bool getConvergence(const SimulatorTimerInterface& timer, const int iteration);

        /// The number of active fluid phases in the model.
        int numPhases() const;

        /// The number of active materials in the model.
        /// This should be equal to the number of material balance
        /// equations.
        int numMaterials() const;

        /// The name of an active material in the model.
        /// It is required that material_index < numMaterials().
        const std::string& materialName(int material_index) const;

        /// Update the scaling factors for mass balance equations
        void updateEquationsScaling();

        /// return the WellModel object
        WellModel& wellModel() { return well_model_; }
        const WellModel& wellModel() const { return well_model_; }

        /// Return reservoir simulation data (for output functionality)
        const SimulatorData& getSimulatorData() const {
            return sd_;
        }

        /// Compute fluid in place.
        /// \param[in]    ReservoirState
        /// \param[in]    FIPNUM for active cells not global cells.
        /// \return fluid in place, number of fip regions, each region contains 5 values which are liquid, vapour, water, free gas and dissolved gas.
        std::vector<V>
        computeFluidInPlace(const ReservoirState& x,
                            const std::vector<int>& fipnum);

    protected:

        // ---------  Types and enums  ---------

        typedef Eigen::Array<double,
                             Eigen::Dynamic,
                             Eigen::Dynamic,
                             Eigen::RowMajor> DataBlock;


        // ---------  Data members  ---------

        const Grid&         grid_;
        const BlackoilPropsAdInterface& fluid_;
        const DerivedGeology&           geo_;
        const RockCompressibility*      rock_comp_props_;
        VFPProperties                   vfp_properties_;
        const NewtonIterationBlackoilInterface&    linsolver_;
        // For each canonical phase -> true if active
        const std::vector<bool>         active_;
        // Size = # active phases. Maps active -> canonical phase indices.
        const std::vector<int>          canph_;
        const std::vector<int>          cells_;  // All grid cells
        HelperOps                       ops_;
        const bool has_disgas_;
        const bool has_vapoil_;

        ModelParameters                 param_;
        bool use_threshold_pressure_;
        V threshold_pressures_by_connection_;

        mutable SimulatorData sd_;
        std::vector<PhasePresence> phaseCondition_;

        // Well Model
        WellModel                       well_model_;

        V isRs_;
        V isRv_;
        V isSg_;

        LinearisedBlackoilResidual residual_;

        /// \brief Whether we print something to std::cout
        bool terminal_output_;
        /// \brief The number of cells of the global grid.
        int global_nc_;

        V pvdt_;
        std::vector<std::string> material_name_;
        std::vector<std::vector<double>> residual_norms_history_;
        double current_relaxation_;
        V dx_old_;

        // rate converter between the surface volume rates and reservoir voidage rates
        RateConverterType rate_converter_;

        // ---------  Protected methods  ---------

        /// Access the most-derived class used for
        /// static polymorphism (CRTP).
        Implementation& asImpl()
        {
            return static_cast<Implementation&>(*this);
        }

        /// Access the most-derived class used for
        /// static polymorphism (CRTP).
        const Implementation& asImpl() const
        {
            return static_cast<const Implementation&>(*this);
        }

        /// return the Well struct in the WellModel
        const Wells& wells() const { return well_model_.wells(); }

        /// return true if wells are available in the reservoir
        bool wellsActive() const { return well_model_.wellsActive(); }

        /// return true if wells are available on this process
        bool localWellsActive() const { return well_model_.localWellsActive(); }

        void
        makeConstantState(SolutionState& state) const;

        SolutionState
        variableState(const ReservoirState& x,
                      const WellState& xw) const;

        std::vector<V>
        variableStateInitials(const ReservoirState& x,
                              const WellState& xw) const;
        void
        variableReservoirStateInitials(const ReservoirState& x,
                                       std::vector<V>& vars0) const;

        std::vector<int>
        variableStateIndices() const;

        SolutionState
        variableStateExtractVars(const ReservoirState& x,
                                 const std::vector<int>& indices,
                                 std::vector<ADB>& vars) const;

        void
        computeAccum(const SolutionState& state,
                     const int            aix  );

        void
        assembleMassBalanceEq(const SolutionState& state);


        IterationReport
        solveWellEq(const std::vector<ADB>& mob_perfcells,
                    const std::vector<ADB>& b_perfcells,
                    SolutionState& state,
                    WellState& well_state);

        void
        addWellContributionToMassBalanceEq(const std::vector<ADB>& cq_s,
                                           const SolutionState& state,
                                           const WellState& xw);

        bool getWellConvergence(const int iteration);

        bool isVFPActive() const;

        std::vector<ADB>
        computePressures(const ADB& po,
                         const ADB& sw,
                         const ADB& so,
                         const ADB& sg) const;

        V
        computeGasPressure(const V& po,
                           const V& sw,
                           const V& so,
                           const V& sg) const;

        std::vector<ADB>
        computeRelPerm(const SolutionState& state) const;

        void
        computeMassFlux(const int               actph ,
                        const V&                transi,
                        const ADB&              kr    ,
                        const ADB&              mu    ,
                        const ADB&              rho    ,
                        const ADB&              p     ,
                        const SolutionState&    state );

        void applyThresholdPressures(ADB& dp);

        ADB
        fluidViscosity(const int               phase,
                       const ADB&              p    ,
                       const ADB&              temp ,
                       const ADB&              rs   ,
                       const ADB&              rv   ,
                       const std::vector<PhasePresence>& cond) const;

        ADB
        fluidReciprocFVF(const int               phase,
                         const ADB&              p    ,
                         const ADB&              temp ,
                         const ADB&              rs   ,
                         const ADB&              rv   ,
                         const std::vector<PhasePresence>& cond) const;

        ADB
        fluidDensity(const int  phase,
                     const ADB& b,
                     const ADB& rs,
                     const ADB& rv) const;

        V
        fluidRsSat(const V&                p,
                   const V&                so,
                   const std::vector<int>& cells) const;

        ADB
        fluidRsSat(const ADB&              p,
                   const ADB&              so,
                   const std::vector<int>& cells) const;

        V
        fluidRvSat(const V&                p,
                   const V&                so,
                   const std::vector<int>& cells) const;

        ADB
        fluidRvSat(const ADB&              p,
                   const ADB&              so,
                   const std::vector<int>& cells) const;

        ADB
        poroMult(const ADB& p) const;

        ADB
        transMult(const ADB& p) const;

        const std::vector<PhasePresence>
        phaseCondition() const {return phaseCondition_;}

        void
        classifyCondition(const ReservoirState& state);


        /// update the primal variable for Sg, Rv or Rs. The Gas phase must
        /// be active to call this method.
        void
        updatePrimalVariableFromState(const ReservoirState& state);

        /// Update the phaseCondition_ member based on the primalVariable_ member.
        /// Also updates isRs_, isRv_ and isSg_;
        void
        updatePhaseCondFromPrimalVariable(const ReservoirState& state);

        // TODO: added since the interfaces of the function are different
        // TODO: for StandardWells and MultisegmentWells
        void
        computeWellConnectionPressures(const SolutionState& state,
                                       const WellState& well_state);

        /// \brief Compute the reduction within the convergence check.
        /// \param[in] B     A matrix with MaxNumPhases columns and the same number rows
        ///                  as the number of cells of the grid. B.col(i) contains the values
        ///                  for phase i.
        /// \param[in] tempV A matrix with MaxNumPhases columns and the same number rows
        ///                  as the number of cells of the grid. tempV.col(i) contains the
        ///                   values
        ///                  for phase i.
        /// \param[in] R     A matrix with MaxNumPhases columns and the same number rows
        ///                  as the number of cells of the grid. B.col(i) contains the values
        ///                  for phase i.
        /// \param[out] R_sum An array of size MaxNumPhases where entry i contains the sum
        ///                   of R for the phase i.
        /// \param[out] maxCoeff An array of size MaxNumPhases where entry i contains the
        ///                   maximum of tempV for the phase i.
        /// \param[out] B_avg An array of size MaxNumPhases where entry i contains the average
        ///                   of B for the phase i.
        /// \param[out] maxNormWell The maximum of the well flux equations for each phase.
        /// \param[in]  nc    The number of cells of the local grid.
        /// \return The total pore volume over all cells.
        double
        convergenceReduction(const Eigen::Array<double, Eigen::Dynamic, Eigen::Dynamic>& B,
                             const Eigen::Array<double, Eigen::Dynamic, Eigen::Dynamic>& tempV,
                             const Eigen::Array<double, Eigen::Dynamic, Eigen::Dynamic>& R,
                             std::vector<double>& R_sum,
                             std::vector<double>& maxCoeff,
                             std::vector<double>& B_avg,
                             std::vector<double>& maxNormWell,
                             int nc) const;

        double dpMaxRel() const { return param_.dp_max_rel_; }
        double dsMax() const { return param_.ds_max_; }
        double drMaxRel() const { return param_.dr_max_rel_; }
        double maxResidualAllowed() const { return param_.max_residual_allowed_; }

    };
} // namespace Opm

#include "BlackoilModelBase_impl.hpp"

#endif // OPM_BLACKOILMODELBASE_HEADER_INCLUDED
