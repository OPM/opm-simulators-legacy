/*
  Copyright 2013 SINTEF ICT, Applied Mathematics.
  Copyright 2015 Dr. Blatt - HPC-Simulation-Software & Services.
  Copyright 2015 NTNU.

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

#ifndef OPM_BLACKOILPROPSADFROMDECK_HEADER_INCLUDED
#define OPM_BLACKOILPROPSADFROMDECK_HEADER_INCLUDED

#include <opm/autodiff/AutoDiffBlock.hpp>
#include <opm/autodiff/BlackoilModelEnums.hpp>

#include <opm/core/props/satfunc/SaturationPropsFromDeck.hpp>
#include <opm/core/props/rock/RockFromDeck.hpp>

#include <opm/material/fluidsystems/BlackOilFluidSystem.hpp>
#include <opm/material/densead/Math.hpp>
#include <opm/material/densead/Evaluation.hpp>

#include <opm/parser/eclipse/Deck/Deck.hpp>
#include <opm/parser/eclipse/EclipseState/EclipseState.hpp>

#include <memory>
#include <array>
#include <vector>

#ifdef HAVE_OPM_GRID
#include <opm/common/utility/platform_dependent/disable_warnings.h>
#include <opm/grid/CpGrid.hpp>
#include <opm/common/utility/platform_dependent/reenable_warnings.h>
#endif

namespace Opm
{
    class PvtInterface;

    /// This class implements the AD-adapted fluid interface for
    /// three-phase black-oil. It requires an input deck from which it
    /// reads all relevant property data.
    ///
    /// Most methods are available in two overloaded versions, one
    /// taking a constant vector and returning the same, and one
    /// taking an AD type and returning the same. Derivatives are not
    /// returned separately by any method, only implicitly with the AD
    /// version of the methods.
    class BlackoilPropsAdFromDeck
    {
        friend class BlackoilPropsDataHandle;
    public:
        typedef BlackOilFluidSystem<double> FluidSystem;
        typedef Opm::GasPvtMultiplexer<double> GasPvt;
        typedef Opm::OilPvtMultiplexer<double> OilPvt;
        typedef Opm::WaterPvtMultiplexer<double> WaterPvt;

        typedef typename SaturationPropsFromDeck::MaterialLawManager MaterialLawManager;

        /// Constructor to create a blackoil properties from an ECL deck.
        ///
        /// The materialLawManager parameter represents the object from opm-material
        /// which handles the creating and updating parameter objects for the capillary
        /// pressure/relperm relations for each grid cell. This object is created
        /// internally for the constructors below, but if it is already available
        /// externally some performance can be gained by creating it only once.
        ///
        /// \param deck The unprocessed ECL deck from opm-parser
        /// \param eclState The processed ECL deck from opm-parser
        /// \param materialLawManager The container for the material law parameter objects
        /// \param grid The grid upon which the simulation is run on.
        /// \param init_rock If true the rock properties (rock compressibility and
        ///                  reference pressure) are read from the deck
        BlackoilPropsAdFromDeck(const Opm::Deck& deck,
                                const Opm::EclipseState& eclState,
                                std::shared_ptr<MaterialLawManager> materialLawManager,
                                const UnstructuredGrid& grid,
                                const bool init_rock = true );

#ifdef HAVE_OPM_GRID
        /// Constructor to create a blackoil properties from an ECL deck.
        ///
        /// The materialLawManager parameter represents the object from opm-material
        /// which handles the creating and updating parameter objects for the capillary
        /// pressure/relperm relations for each grid cell. This object is created
        /// internally for the constructors below, but if it is already available
        /// externally some performance can be gained by creating it only once.
        ///
        /// \param deck The unprocessed ECL deck from opm-parser
        /// \param eclState The processed ECL deck from opm-parser
        /// \param materialLawManager The container for the material law parameter objects
        /// \param grid The grid upon which the simulation is run on.
        /// \param init_rock If true the rock properties (rock compressibility and
        ///                  reference pressure) are read from the deck
        BlackoilPropsAdFromDeck(const Opm::Deck& deck,
                                const Opm::EclipseState& eclState,
                                std::shared_ptr<MaterialLawManager> materialLawManager,
                                const Dune::CpGrid& grid,
                                const bool init_rock = true );
#endif

        /// Constructor to create a blackoil properties from an ECL deck.
        ///
        /// \param deck The unprocessed ECL deck from opm-parser
        /// \param eclState The processed ECL deck from opm-parser
        /// \param grid The grid upon which the simulation is run on.
        /// \param init_rock If true the rock properties (rock compressibility and
        ///                  reference pressure) are read from the deck
        BlackoilPropsAdFromDeck(const Opm::Deck& deck,
                                const Opm::EclipseState& eclState,
                                const UnstructuredGrid& grid,
                                const bool init_rock = true );

#ifdef HAVE_OPM_GRID
        /// Constructor to create a blackoil properties from an ECL deck.
        ///
        /// \param deck The unprocessed ECL deck from opm-parser
        /// \param eclState The processed ECL deck from opm-parser
        /// \param grid The grid upon which the simulation is run on.
        /// \param init_rock If true the rock properties (rock compressibility and
        ///                  reference pressure) are read from the deck
        BlackoilPropsAdFromDeck(const Opm::Deck& deck,
                                const Opm::EclipseState& eclState,
                                const Dune::CpGrid& grid,
                                const bool init_rock = true );
#endif

        /// \brief Constructor to create properties for a subgrid
        ///
        /// This copies all properties that are not dependant on the
        /// grid size from an existing properties object
        /// and the number of cells. All properties that do not depend
        /// on the grid dimension will be copied. For the rest will have
        /// the correct size but the values will be undefined.
        ///
        /// \param props            The property object to copy from.
        /// \param materialLawManager The container for the material law parameter objects.
        ///                           Initialized only for the subgrid
        /// \param number_of_cells  The number of cells of the subgrid.
        BlackoilPropsAdFromDeck(const BlackoilPropsAdFromDeck& props,
                                std::shared_ptr<MaterialLawManager> materialLawManager,
                                const int number_of_cells);


        ////////////////////////////
        //      Rock interface    //
        ////////////////////////////

        /// \return   D, the number of spatial dimensions.
        int numDimensions() const;

        /// \return   N, the number of cells.
        int numCells() const;

        /// Return an array containing the PVT table index for each
        /// grid cell
        virtual const int* cellPvtRegionIndex() const
        { return &cellPvtRegionIdx_[0]; }

        /// \return   Array of N porosity values.
        const double* porosity() const;

        /// \return   Array of ND^2 permeability values.
        ///           The D^2 permeability values for a cell are organized as a matrix,
        ///           which is symmetric (so ordering does not matter).
        const double* permeability() const;


        ////////////////////////////
        //      Fluid interface   //
        ////////////////////////////

        typedef AutoDiffBlock<double> ADB;
        typedef ADB::V V;
        typedef std::vector<int> Cells;

        /// \return   Number of active phases (also the number of components).
        int numPhases() const;

        /// \return   Object describing the active phases.
        PhaseUsage phaseUsage() const;

        // ------ Density ------

        /// Densities of stock components at surface conditions.
        /// \param[in] phaseIdx
        /// \param[in] cells  Array of n cell indices to be associated with the pressure values.
        /// \return Array of n density values for phase given by phaseIdx.
        V surfaceDensity(const int phaseIdx , const Cells& cells) const;


        // ------ Viscosity ------

        /// Water viscosity.
        /// \param[in]  pw     Array of n water pressure values.
        /// \param[in]  T      Array of n temperature values.
        /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
        /// \return            Array of n viscosity values.
        ADB muWat(const ADB& pw,
                  const ADB& T,
                  const Cells& cells) const;

        /// Oil viscosity.
        /// \param[in]  po     Array of n oil pressure values.
        /// \param[in]  T      Array of n temperature values.
        /// \param[in]  rs     Array of n gas solution factor values.
        /// \param[in]  cond   Array of n objects, each specifying which phases are present with non-zero saturation in a cell.
        /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
        /// \return            Array of n viscosity values.
        ADB muOil(const ADB& po,
                  const ADB& T,
                  const ADB& rs,
                  const std::vector<PhasePresence>& cond,
                  const Cells& cells) const;

        /// Gas viscosity.
        /// \param[in]  pg     Array of n gas pressure values.
        /// \param[in]  T      Array of n temperature values.
        /// \param[in]  rv     Array of n vapor oil/gas ratios.
        /// \param[in]  cond   Array of n objects, each specifying which phases are present with non-zero saturation in a cell.
        /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
        /// \return            Array of n viscosity values.
        ADB muGas(const ADB& pg,
                  const ADB& T,
                  const ADB& rv,
                  const std::vector<PhasePresence>& cond,
                  const Cells& cells) const;

        // ------ Formation volume factor (b) ------

        /// Water formation volume factor.
        /// \param[in]  pw     Array of n water pressure values.
        /// \param[in]  T      Array of n temperature values.
        /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
        /// \return            Array of n formation volume factor values.
        ADB bWat(const ADB& pw,
                 const ADB& T,
                 const Cells& cells) const;

        /// Oil formation volume factor.
        /// \param[in]  po     Array of n oil pressure values.
        /// \param[in]  T      Array of n temperature values.
        /// \param[in]  rs     Array of n gas solution factor values.
        /// \param[in]  cond   Array of n objects, each specifying which phases are present with non-zero saturation in a cell.
        /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
        /// \return            Array of n formation volume factor values.
        ADB bOil(const ADB& po,
                 const ADB& T,
                 const ADB& rs,
                 const std::vector<PhasePresence>& cond,
                 const Cells& cells) const;

        /// Gas formation volume factor.
        /// \param[in]  pg     Array of n gas pressure values.
        /// \param[in]  T      Array of n temperature values.
        /// \param[in]  rv     Array of n vapor oil/gas ratio
        /// \param[in]  cond   Array of n objects, each specifying which phases are present with non-zero saturation in a cell.
        /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
        /// \return            Array of n formation volume factor values.
        ADB bGas(const ADB& pg,
                 const ADB& T,
                 const ADB& rv,
                 const std::vector<PhasePresence>& cond,
                 const Cells& cells) const;

        // ------ Rs bubble point curve ------

        /// Bubble point curve for Rs as function of oil pressure.
        /// \param[in]  po     Array of n oil pressure values.
        /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
        /// \return            Array of n bubble point values for Rs.
        V rsSat(const V& po,
                const Cells& cells) const;

        /// Bubble point curve for Rs as function of oil pressure.
        /// \param[in]  po     Array of n oil pressure values.
        /// \param[in]  so     Array of n oil saturation values.
        /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
        /// \return            Array of n bubble point values for Rs.
        V rsSat(const V& po,
                const V& so,
                const Cells& cells) const;

        /// Bubble point curve for Rs as function of oil pressure.
        /// \param[in]  po     Array of n oil pressure values.
        /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
        /// \return            Array of n bubble point values for Rs.
        ADB rsSat(const ADB& po,
                  const Cells& cells) const;

        /// Bubble point curve for Rs as function of oil pressure.
        /// \param[in]  po     Array of n oil pressure values.
        /// \param[in]  so     Array of n oil saturation values.
        /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
        /// \return            Array of n bubble point values for Rs.
        ADB rsSat(const ADB& po,
                  const ADB& so,
                  const Cells& cells) const;

        // ------ Rv condensation curve ------

        /// Condensation curve for Rv as function of oil pressure.
        /// \param[in]  po     Array of n oil pressure values.
        /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
        /// \return            Array of n condensation point values for Rv.
        ADB rvSat(const ADB& po,
                  const Cells& cells) const;

        /// Condensation curve for Rv as function of oil pressure.
        /// \param[in]  po     Array of n oil pressure values.
        /// \param[in]  so     Array of n oil saturation values.
        /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
        /// \return            Array of n condensation point values for Rv.
        ADB rvSat(const ADB& po,
                  const ADB& so,
                  const Cells& cells) const;

        // ------ Relative permeability ------

        /// Relative permeabilities for all phases.
        /// \param[in]  sw     Array of n water saturation values.
        /// \param[in]  so     Array of n oil saturation values.
        /// \param[in]  sg     Array of n gas saturation values.
        /// \param[in]  cells  Array of n cell indices to be associated with the saturation values.
        /// \return            An std::vector with 3 elements, each an array of n relperm values,
        ///                    containing krw, kro, krg. Use PhaseIndex for indexing into the result.
        std::vector<ADB> relperm(const ADB& sw,
                                 const ADB& so,
                                 const ADB& sg,
                                 const Cells& cells) const;

        /// Capillary pressure for all phases.
        /// \param[in]  sw     Array of n water saturation values.
        /// \param[in]  so     Array of n oil saturation values.
        /// \param[in]  sg     Array of n gas saturation values.
        /// \param[in]  cells  Array of n cell indices to be associated with the saturation values.
        /// \return            An std::vector with 3 elements, each an array of n capillary pressure values,
        ///                    containing the offsets for each p_g, p_o, p_w. The capillary pressure between
        ///                    two arbitrary phases alpha and beta is then given as p_alpha - p_beta.
        std::vector<ADB> capPress(const ADB& sw,
                                  const ADB& so,
                                  const ADB& sg,
                                  const Cells& cells) const;
                                  
        /// Saturation update for hysteresis behavior.
        /// \param[in]  cells       Array of n cell indices to be associated with the saturation values.
        void updateSatHyst(const std::vector<double>& saturation,
                           const std::vector<int>& cells);

        /// Set gas-oil hysteresis parameters
        /// \param[in]  pcswmdc  Vector of hysteresis parameters (@see EclHysteresisTwoPhaseLawParams::pcSwMdc(...))
        /// \param[in]  krnswdc  Vector of hysteresis parameters (@see EclHysteresisTwoPhaseLawParams::krnSwMdc(...))
        void setGasOilHystParams(const std::vector<double>& pcswmdc,
                                 const std::vector<double>& krnswdc,
                                 const std::vector<int>& cells);

        /// Get gas-oil hysteresis parameters
        /// \param[in]  pcswmdc  Vector of hysteresis parameters (@see EclHysteresisTwoPhaseLawParams::pcSwMdc(...))
        /// \param[in]  krnswdc  Vector of hysteresis parameters (@see EclHysteresisTwoPhaseLawParams::krnSwMdc(...))
        void getGasOilHystParams(std::vector<double>& pcswmdc,
                                 std::vector<double>& krnswdc,
                                 const std::vector<int>& cells) const;

        /// Set oil-water hysteresis parameters
        /// \param[in]  pcswmdc  Vector of hysteresis parameters (@see EclHysteresisTwoPhaseLawParams::pcSwMdc(...))
        /// \param[in]  krnswdc  Vector of hysteresis parameters (@see EclHysteresisTwoPhaseLawParams::krnSwMdc(...))
        void setOilWaterHystParams(const std::vector<double>& pcswmdc,
                                   const std::vector<double>& krnswdc,
                                   const std::vector<int>& cells);

        /// Set oil-water hysteresis parameters
        /// \param[in]  pcswmdc  Vector of hysteresis parameters (@see EclHysteresisTwoPhaseLawParams::pcSwMdc(...))
        /// \param[in]  krnswdc  Vector of hysteresis parameters (@see EclHysteresisTwoPhaseLawParams::krnSwMdc(...))
        void getOilWaterHystParams(std::vector<double>& pcswmdc,
                                   std::vector<double>& krnswdc,
                                   const std::vector<int>& cells) const;

        /// Update for max oil saturation.
        /// \param[in] saturation Saturations for all phases
        void updateSatOilMax(const std::vector<double>& saturation);

        /// Returns the max oil saturation vector
        const std::vector<double>& satOilMax() const;

        /// Force set max oil saturation (used for restarting)
        /// \param[in] max_sat Max oil saturations.
        /// Note that this is a vector of *only* oil saturations (no other phases)
        /// @see The similar function updateSatOilMax(const std::vector<double>& saturation)
        /// @see satOilMax()
        void setSatOilMax(const std::vector<double>& max_sat);

        /// Returns the bubble point pressures
        std::vector<double> bubblePointPressure(const Cells& cells,
                const V& T,
                const V& rs) const;

        /// Returns the dew point pressures
        std::vector<double> dewPointPressure(const Cells& cells,
                const V& T,
                const V& rv) const;

        /// Set capillary pressure scaling according to pressure diff. and initial water saturation.
        /// \param[in]  saturation Array of n*numPhases saturation values.
        /// \param[in]  pc         Array of n*numPhases capillary pressure values.
        void setSwatInitScaling(const std::vector<double>& saturation,
                      const std::vector<double>& pc);

        /// Obtain the scaled critical oil in gas saturation values.
        /// \param[in]  cells  Array of cell indices.
        /// \return Array of critical oil in gas saturaion values.
        V scaledCriticalOilinGasSaturations(const Cells& cells) const;

        /// Obtain the scaled critical gas saturation values.
        /// \param[in]  cells  Array of cell indices.
        /// \return Array of scaled critical gas saturaion values.
        V scaledCriticalGasSaturations(const Cells& cells) const;

        /// Direct access to lower-level water pvt props.
        const WaterPvt& waterProps() const
        {
            return FluidSystem::waterPvt();
        }

        /// Direct access to lower-level oil pvt props.
        const OilPvt& oilProps() const
        {
            return FluidSystem::oilPvt();
        }

        /// Direct access to lower-level gas pvt props.
        const GasPvt& gasProps() const
        {
            return FluidSystem::gasPvt();
        }

        /// Direct access to lower-level saturation functions.
        const MaterialLawManager& materialLaws() const
        {
            return *materialLawManager_;
        }

        // Direct access to pvt region indices.
        const std::vector<int>& pvtRegions() const
        {
            return cellPvtRegionIdx_;
        }


    private:
        /// Initializes the properties.
        void init(const Opm::Deck& deck,
                  const Opm::EclipseState& eclState,
                  std::shared_ptr<MaterialLawManager> materialLawManager,
                  int number_of_cells,
                  const int* global_cell,
                  const int* cart_dims,
                  const bool init_rock);

        /// Correction to rs/rv according to kw VAPPARS
        void applyVap(V& r,
                      const V& so,
                      const std::vector<int>& cells,
                      const double vap) const;

        void applyVap(ADB& r,
                      const ADB& so,
                      const std::vector<int>& cells,
                      const double vap) const;

        RockFromDeck rock_;

        // This has to be a shared pointer as we must
        // be able to make a copy of *this in the parallel case.
        std::shared_ptr<MaterialLawManager> materialLawManager_;
        std::shared_ptr<SaturationPropsFromDeck> satprops_;

        PhaseUsage phase_usage_;
        // bool has_vapoil_;
        // bool has_disgas_;

        // The PVT region which is to be used for each cell
        std::vector<int> cellPvtRegionIdx_;

        // VAPPARS
        double vap1_;
        double vap2_;
        std::vector<double> satOilMax_;
        double vap_satmax_guard_;  //Threshold value to promote stability
    };
} // namespace Opm

#endif // OPM_BLACKOILPROPSADFROMDECK_HEADER_INCLUDED
