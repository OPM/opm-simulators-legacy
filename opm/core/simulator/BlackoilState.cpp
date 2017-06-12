#include "BlackoilState.hpp"
#include <opm/common/util/numeric/cmp.hpp>
#include <opm/core/props/BlackoilPropertiesInterface.hpp>
#include <opm/core/simulator/WellState.hpp>
#include <opm/output/data/Wells.hpp>


using namespace Opm;


const std::string BlackoilState::GASOILRATIO = "GASOILRATIO";
const std::string BlackoilState::RV = "RV";
const std::string BlackoilState::SURFACEVOL = "SURFACEVOL";
const std::string BlackoilState::SSOL = "SSOL";
const std::string BlackoilState::POLYMER = "POLYMER";


BlackoilState::BlackoilState( size_t num_cells , size_t num_faces , size_t num_phases)
    : SimulationDataContainer( num_cells , num_faces , num_phases)
{
    registerCellData( GASOILRATIO , 1 );
    registerCellData( RV, 1 );
    registerCellData( SURFACEVOL, num_phases );
    registerCellData( SSOL , 1 );
    registerCellData( POLYMER , 1 );
    setBlackoilStateReferencePointers();
}

BlackoilState::BlackoilState( const BlackoilState& other )
    : SimulationDataContainer(other),
      hydrocarbonstate_(other.hydroCarbonState())
{
    setBlackoilStateReferencePointers();

}

BlackoilState& BlackoilState::operator=( const BlackoilState& other )
{
    SimulationDataContainer::operator=(other);
    setBlackoilStateReferencePointers();
    hydrocarbonstate_ = other.hydroCarbonState();
    return *this;
}

void BlackoilState::setBlackoilStateReferencePointers()
{
    // This sets the reference pointers for the fast
    // accessors, the fields must have been created first.
    gasoilratio_ref_ = &getCellData(GASOILRATIO);
    rv_ref_          = &getCellData(RV);
    surfacevol_ref_  = &getCellData(SURFACEVOL);
}
