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

#ifndef OPM_BLACKOILPROPERTIESBASIC_HEADER_INCLUDED
#define OPM_BLACKOILPROPERTIESBASIC_HEADER_INCLUDED


#include <opm/core/props/BlackoilPropertiesInterface.hpp>
#include <opm/core/props/rock/RockBasic.hpp>
#include <opm/core/props/pvt/PvtPropertiesBasic.hpp>
#include <opm/core/props/satfunc/SaturationPropsBasic.hpp>
#include <opm/core/utility/parameters/ParameterGroup.hpp>

namespace Opm
{

    /// Concrete class implementing the blackoil property interface,
    /// reading all necessary input from parameters.
    class BlackoilPropertiesBasic : public BlackoilPropertiesInterface
    {
    public:
        /// Construct from parameters.
        /// The following parameters are accepted (defaults):
        ///    - num_phases       (2)        -- Must be 1 or 2.
        ///    - relperm_func     ("Linear") -- Must be "Constant", "Linear" or "Quadratic".
        ///    - rho1, rho2, rho3 (1.0e3)    -- Density in kg/m^3
        ///    - mu1, mu2, mu3    (1.0)      -- Viscosity in cP
        ///    - porosity         (1.0)      -- Porosity
        ///    - permeability     (100.0)    -- Permeability in mD
        BlackoilPropertiesBasic(const ParameterGroup& param,
                                const int dim,
                                const int num_cells);

        /// Destructor.
        virtual ~BlackoilPropertiesBasic();


        // ---- Rock interface ----

        /// \return   D, the number of spatial dimensions.
        virtual int numDimensions() const;

        /// \return   N, the number of cells.
        virtual int numCells() const;

        /// Return an array containing the PVT table index for each
        /// grid cell
        virtual const int* cellPvtRegionIndex() const
        { return NULL; }

        /// \return   Array of N porosity values.
        virtual const double* porosity() const;

        /// \return   Array of ND^2 permeability values.
        ///           The D^2 permeability values for a cell are organized as a matrix,
        ///           which is symmetric (so ordering does not matter).
        virtual const double* permeability() const;


        // ---- Fluid interface ----

        /// \return   P, the number of phases (also the number of components).
        virtual int numPhases() const;

        /// \return   Object describing the active phases.
        virtual PhaseUsage phaseUsage() const;

        /// \param[in]  n      Number of data points.
        /// \param[in]  p      Array of n pressure values.
        /// \param[in]  T      Array of n temperature values.
        /// \param[in]  z      Array of nP surface volume values.
        /// \param[in]  cells  Array of n cell indices to be associated with the p and z values.
        /// \param[out] mu     Array of nP viscosity values, array must be valid before calling.
        /// \param[out] dmudp  If non-null: array of nP viscosity derivative values,
        ///                    array must be valid before calling.
        virtual void viscosity(const int n,
                               const double* p,
                               const double* T,
                               const double* z,
                               const int* cells,
                               double* mu,
                               double* dmudp) const;

        /// \param[in]  n      Number of data points.
        /// \param[in]  p      Array of n pressure values.
        /// \param[in]  T      Array of n temperature values.
        /// \param[in]  z      Array of nP surface volume values.
        /// \param[in]  cells  Array of n cell indices to be associated with the p and z values.
        /// \param[out] A      Array of nP^2 values, array must be valid before calling.
        ///                    The P^2 values for a cell give the matrix A = RB^{-1} which
        ///                    relates z to u by z = Au. The matrices are output in Fortran order.
        /// \param[out] dAdp   If non-null: array of nP^2 matrix derivative values,
        ///                    array must be valid before calling. The matrices are output
        ///                    in Fortran order.
        virtual void matrix(const int n,
                            const double* p,
                            const double* T,
                            const double* z,
                            const int* cells,
                            double* A,
                            double* dAdp) const;


        /// Densities of stock components at reservoir conditions.
        /// \param[in]  n      Number of data points.
        /// \param[in]  A      Array of nP^2 values, where the P^2 values for a cell give the
        ///                    matrix A = RB^{-1} which relates z to u by z = Au. The matrices
        ///                    are assumed to be in Fortran order, and are typically the result
        ///                    of a call to the method matrix().
        /// \param[in]  cells  The index of the grid cell of each data point.
        /// \param[out] rho    Array of nP density values, array must be valid before calling.
        virtual void density(const int n,
                             const double* A,
                             const int* cells,
                             double* rho) const;

        /// Densities of stock components at surface conditions.
        /// \param[in]  cellIdx  The index of the cell for which the surface density ought to be calculated
        /// \return Array of P density values.
        virtual const double* surfaceDensity(int cellIdx = 0) const;

        /// \param[in]  n      Number of data points.
        /// \param[in]  s      Array of nP saturation values.
        /// \param[in]  cells  Array of n cell indices to be associated with the s values.
        /// \param[out] kr     Array of nP relperm values, array must be valid before calling.
        /// \param[out] dkrds  If non-null: array of nP^2 relperm derivative values,
        ///                    array must be valid before calling.
        ///                    The P^2 derivative matrix is
        ///                           m_{ij} = \frac{dkr_i}{ds^j},
        ///                    and is output in Fortran order (m_00 m_10 m_20 m_01 ...)
        virtual void relperm(const int n,
                             const double* s,
                             const int* cells,
                             double* kr,
                             double* dkrds) const;


        /// \param[in]  n      Number of data points.
        /// \param[in]  s      Array of nP saturation values.
        /// \param[in]  cells  Array of n cell indices to be associated with the s values.
        /// \param[out] pc     Array of nP capillary pressure values, array must be valid before calling.
        /// \param[out] dpcds  If non-null: array of nP^2 derivative values,
        ///                    array must be valid before calling.
        ///                    The P^2 derivative matrix is
        ///                           m_{ij} = \frac{dpc_i}{ds^j},
        ///                    and is output in Fortran order (m_00 m_10 m_20 m_01 ...)
        virtual void capPress(const int n,
                              const double* s,
                              const int* cells,
                              double* pc,
                              double* dpcds) const;


        /// Obtain the range of allowable saturation values.
	    /// In cell cells[i], saturation of phase p is allowed to be
	    /// in the interval [smin[i*P + p], smax[i*P + p]].
        /// \param[in]  n      Number of data points.
        /// \param[in]  cells  Array of n cell indices.
        /// \param[out] smin   Array of nP minimum s values, array must be valid before calling.
        /// \param[out] smax   Array of nP maximum s values, array must be valid before calling.
        virtual void satRange(const int n,
                              const int* cells,
                              double* smin,
                              double* smax) const;


        /// Update capillary pressure scaling according to pressure diff. and initial water saturation.
        /// \param[in]     cell   Cell index.
        /// \param[in]     pcow   P_oil - P_water.
        /// \param[in/out] swat   Water saturation. / Possibly modified Water saturation.      
        virtual  void swatInitScaling(const int cell,
                                      const double pcow, 
                                      double & swat);


    private:
        RockBasic rock_;
	PvtPropertiesBasic pvt_;
        SaturationPropsBasic satprops_;
    };



} // namespace Opm


#endif // OPM_BLACKOILPROPERTIESBASIC_HEADER_INCLUDED
