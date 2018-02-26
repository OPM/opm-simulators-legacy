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

    class ParameterGroup;

    /// Solver parameters for the BlackoilModel.
    struct BlackoilModelParameters
    {
        /// Max relative change in pressure in single iteration.
        double dp_max_rel_;
        /// Max absolute change in saturation in single iteration.
        double ds_max_;
        /// Max relative change in gas-oil or oil-gas ratio in single iteration.
        double dr_max_rel_;
        /// Max relative change in bhp in single iteration.
        double dbhp_max_rel_;
        /// Max absolute change in well volume fraction in single iteration.
        double dwell_fraction_max_;
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
        /// Tolerance for the pressure equations for multisegment wells
        double tolerance_pressure_ms_wells_;
        /// Maximum pressure change over an iteratio for ms wells
        double max_pressure_change_ms_wells_;

        /// Whether to use inner iterations for ms wells
        bool use_inner_iterations_ms_wells_;

        /// Maximum inner iteration number for ms wells
        int max_inner_iter_ms_wells_;

        /// Maximum iteration number of the well equation solution
        int max_welleq_iter_;

        /// Tolerance for time step in seconds where single precision can be used
        /// for solving for the Jacobian
        double maxSinglePrecisionTimeStep_;

        /// Maximum number of Newton iterations before we give up on the CNV convergence criterion
        int max_strict_iter_;

        /// Solve well equation initially
        bool solve_welleq_initially_;

        /// Update scaling factors for mass balance equations
        bool update_equations_scaling_;

        /// Try to detect oscillation or stagnation.
        bool use_update_stabilization_;

        /// Whether to use MultisegmentWell to handle multisegment wells
        /// it is something temporary before the multisegment well model is considered to be
        /// well developed and tested.
        /// if it is false, we will handle multisegment wells as standard wells, which will be
        /// the default behavoir for the moment. Later, we might set it to be true by default if necessary
        bool use_multisegment_well_;

        /// The file name of the deck
        std::string deck_file_name_;

        // Whether to add influences of wells between cells to the matrix
        bool matrix_add_well_contributions_;

        /// Construct from user parameters or defaults.
        explicit BlackoilModelParameters( const ParameterGroup& param );

        /// Construct with default parameters.
        BlackoilModelParameters();

        /// Set default parameters.
        void reset();
    };

} // namespace Opm

#endif // OPM_BLACKOILMODELPARAMETERS_HEADER_INCLUDED
