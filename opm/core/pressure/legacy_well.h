/*
  Copyright 2010 SINTEF ICT, Applied Mathematics.

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

#ifndef OPM_LEGACY_WELL_HEADER_INCLUDED
#define OPM_LEGACY_WELL_HEADER_INCLUDED

/**
 * \file
 * Deprecated (and obsolescent) well definition.  Still in use by
 * the hybridized pressure solvers.
 */

#ifdef __cplusplus
extern "C" {
#endif


/**
 * Well taxonomy.
 */
enum well_type    { INJECTOR, PRODUCER };

/**
 * Control types recognised in system.
 */
enum well_control { BHP     , RATE     };

/**
 * Compositions recognised in injection wells.
 */
enum surface_component { WATER = 0, OIL = 1, GAS = 2 };

/**
 * Basic representation of well topology.
 */
struct LegacyWellCompletions {
    int  number_of_wells; /**< Number of wells. */
    int *well_connpos;    /**< Well topology start pointers. */
    int *well_cells;      /**< Well connections */
};

/**
 * Basic representation of well controls.
 */
struct LegacyWellControls {
    enum well_type    *type;    /**< Individual well taxonomy */
    enum well_control *ctrl;    /**< Individual well controls */
    double            *target;  /**< Control target */
    double            *zfrac;   /**< Surface injection composition */
};

/**
 * Dynamic discretisation data relating well to flow in reservoir.
 */
struct completion_data {
    double *WI;       /**< Well indices */
    double *gpot;     /**< Gravity potential */
    double *A;        /**< \f$RB^{-1}\f$ for compressible flows. */
    double *phasemob; /**< Phase mobility, per connection. */
};

/**
 * Convenience type alias to preserve backwards compatibility in
 * well topology definitions used by hybridised pressure solver.
 */
typedef struct LegacyWellCompletions well_t;

/**
 * Convenience type alias to preserve backwards compatiblity in
 * well control definitions used by hybridised pressure solver.
 */
typedef struct LegacyWellControls    well_control_t;

#ifdef __cplusplus
}
#endif

#endif /* OPM_LEGACY_WELL_HEADER_INCLUDED */
