#include "BlackoilState.hpp"
#include <opm/common/util/numeric/cmp.hpp>
#include <opm/core/props/BlackoilPropertiesInterface.hpp>


using namespace Opm;

void
BlackoilState::init(int number_of_cells, int number_of_phases, int num_phases)
{
   SimulatorState::init(number_of_cells, number_of_phases, num_phases);

   // register cell data in base class
   gorId_ = SimulatorState::registerCellData( "GASOILRATIO", 1 );
   rvId_  = SimulatorState::registerCellData( "RV", 1 );

   // surfvolumes intentionally empty, left to initBlackoilSurfvol
   surfaceVolId_ = SimulatorState::registerCellData( "SURFACEVOL", 0 );
}

void
BlackoilState::init(const UnstructuredGrid& g, int num_phases)
{
    init(g.number_of_cells, g.number_of_faces, num_phases);
}

bool
BlackoilState::equals(const SimulatorState& other,
                      double epsilon) const {
    const BlackoilState* that = dynamic_cast <const BlackoilState*> (&other);
    bool equal = that != 0;
    equal = equal && SimulatorState::equals (other, epsilon);
    equal = equal && cmp::vector_equal(this->surfacevol(),
				       that->surfacevol(),
				       cmp::default_abs_epsilon,
				       epsilon);

    equal = equal && cmp::vector_equal(this->gasoilratio(),
				       that->gasoilratio(),
				       cmp::default_abs_epsilon,
				       epsilon);

    equal = equal && cmp::vector_equal(this->rv(),
				       that->rv(),
				       cmp::default_abs_epsilon,
				       epsilon);
    return equal;
}
