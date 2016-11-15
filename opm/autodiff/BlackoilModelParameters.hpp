/*
  Copyright 2015 SINTEF ICT, Applied Mathematics.

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

#ifndef OPM_BLACKOILMODELPARAMETERS_HEADER_INCLUDED
#define OPM_BLACKOILMODELPARAMETERS_HEADER_INCLUDED

#include <string>

namespace Opm
{

    namespace parameter { class ParameterGroup; }

    /// Solver parameters for the BlackoilModel.
    struct BlackoilModelParameters
    {
        /// Max relative change in pressure in single iteration.
        double dp_max_rel_;
        /// Max absolute change in saturation in single iteration.
        double ds_max_;
        /// Max relative change in gas-oil or oil-gas ratio in single iteration.
        double dr_max_rel_;
        /// Absolute max limit for residuals.
        double max_residual_allowed_;
        /// Relative mass balance tolerance (total mass balance error).
        double tolerance_mb_;
        /// Local convergence tolerance (max of local saturation errors).
        double tolerance_cnv_;
        /// Well convergence tolerance.
        double tolerance_wells_;
        /// Tolerance for the well control equations
        //  TODO: it might need to distinguish between rate control and pressure control later
        double tolerance_well_control_;

        /// Solve well equation initially
        bool solve_welleq_initially_;

        /// Update scaling factors for mass balance equations
        bool update_equations_scaling_;

        /// Compute well potentials, needed to calculate default guide rates for group
        /// controlled wells
        bool compute_well_potentials_;

        /// Try to detect oscillation or stagnation.
        bool use_update_stabilization_;

        // The file name of the deck
        std::string deck_file_name_;

        /// Construct from user parameters or defaults.
        explicit BlackoilModelParameters( const parameter::ParameterGroup& param );

        /// Construct with default parameters.
        BlackoilModelParameters();

        /// Set default parameters.
        void reset();
    };

} // namespace Opm

#endif // OPM_BLACKOILMODELPARAMETERS_HEADER_INCLUDED
