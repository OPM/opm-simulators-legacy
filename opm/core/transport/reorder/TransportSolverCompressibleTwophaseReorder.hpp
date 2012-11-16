/*
  Copyright 2012 SINTEF ICT, Applied Mathematics.

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

#ifndef OPM_TRANSPORTMODELCOMPRESSIBLETWOPHASE_HEADER_INCLUDED
#define OPM_TRANSPORTMODELCOMPRESSIBLETWOPHASE_HEADER_INCLUDED

#include <opm/core/transport/reorder/TransportModelInterface.hpp>
#include <vector>

struct UnstructuredGrid;

namespace Opm
{

    class BlackoilPropertiesInterface;

    /// Implements a reordering transport solver for compressible,
    /// non-miscible two-phase flow.
    class TransportModelCompressibleTwophase : public TransportModelInterface
    {
    public:
        /// Construct solver.
        /// \param[in] grid      A 2d or 3d grid.
        /// \param[in] props     Rock and fluid properties.
        /// \param[in] tol       Tolerance used in the solver.
        /// \param[in] maxit     Maximum number of non-linear iterations used.
        TransportModelCompressibleTwophase(const UnstructuredGrid& grid,
                                           const Opm::BlackoilPropertiesInterface& props,
                                           const double tol,
                                           const int maxit);

        /// Solve for saturation at next timestep.
        /// \param[in] darcyflux         Array of signed face fluxes.
        /// \param[in] pressure          Array of cell pressures
        /// \param[in] surfacevol0       Array of surface volumes at start of timestep
        /// \param[in] porevolume0       Array of pore volumes at start of timestep.
        /// \param[in] porevolume        Array of pore volumes at end of timestep.
        /// \param[in] source            Transport source term.
        /// \param[in] dt                Time step.
        /// \param[in, out] saturation   Phase saturations.
        /// \param[in, out] surfacevol   Surface volume densities for each phase.
        void solve(const double* darcyflux,
                   const double* pressure,
                   const double* porevolume0,
                   const double* porevolume,
                   const double* source,
                   const double dt,
                   std::vector<double>& saturation,
                   std::vector<double>& surfacevol);

        /// Initialise quantities needed by gravity solver.
        /// \param[in] grav    Gravity vector
        void initGravity(const double* grav);

        /// Solve for gravity segregation.
        /// This uses a column-wise nonlinear Gauss-Seidel approach.
        /// It assumes that the input columns contain cells in a single
        /// vertical stack, that do not interact with other columns (for
        /// gravity segregation.
        /// \param[in] columns           Vector of cell-columns.
        /// \param[in] dt                Time step.
        /// \param[in, out] saturation   Phase saturations.
        /// \param[in, out] surfacevol   Surface volume densities for each phase.
        void solveGravity(const std::vector<std::vector<int> >& columns,
                          const double dt,
                          std::vector<double>& saturation,
                          std::vector<double>& surfacevol);

    private:
        virtual void solveSingleCell(const int cell);
        virtual void solveMultiCell(const int num_cells, const int* cells);
        void solveSingleCellGravity(const std::vector<int>& cells,
                                    const int pos,
                                    const double* gravflux);
        int solveGravityColumn(const std::vector<int>& cells);
        void initGravityDynamic();

    private:
        const UnstructuredGrid& grid_;
        const BlackoilPropertiesInterface& props_;
        std::vector<int> allcells_;
        std::vector<double> visc_;
        std::vector<double> A_;
        std::vector<double> smin_;
        std::vector<double> smax_;
        double tol_;
        double maxit_;

        const double* darcyflux_;   // one flux per grid face
        const double* surfacevol0_; // one per phase per cell
        const double* porevolume0_; // one volume per cell
        const double* porevolume_;  // one volume per cell
        const double* source_;      // one source per cell
        double dt_;
        std::vector<double> saturation_;        // P (= num. phases) per cell
        std::vector<double> fractionalflow_;  // = m[0]/(m[0] + m[1]) per cell
        // For gravity segregation.
        const double* gravity_;
        std::vector<double> trans_;
        std::vector<double> density_;
        std::vector<double> gravflux_;
        std::vector<double> mob_;
        std::vector<double> s0_;

        // Storing the upwind and downwind graphs for experiments.
        std::vector<int> ia_upw_;
        std::vector<int> ja_upw_;
        std::vector<int> ia_downw_;
        std::vector<int> ja_downw_;

        struct Residual;
        double fracFlow(double s, int cell) const;

        struct GravityResidual;
        void mobility(double s, int cell, double* mob) const;
    };

} // namespace Opm

#endif // OPM_TRANSPORTMODELCOMPRESSIBLETWOPHASE_HEADER_INCLUDED
