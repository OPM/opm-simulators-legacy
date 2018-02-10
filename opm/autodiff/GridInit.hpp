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

#ifndef OPM_GRIDINIT_HEADER_INCLUDED
#define OPM_GRIDINIT_HEADER_INCLUDED

#include <opm/parser/eclipse/Deck/Deck.hpp>
#include <opm/parser/eclipse/EclipseState/EclipseState.hpp>
#include <opm/grid/GridManager.hpp>

#if HAVE_OPM_GRID
#include <opm/grid/polyhedralgrid.hh>
#include <opm/grid/CpGrid.hpp>
#endif


namespace Opm
{

    /// A class intended to give a generic interface to
    /// initializing and accessing UnstructuredGrid and CpGrid,
    /// using specialized templates to accomplish this.
    template <class Grid>
    class GridInit
    {
    public:
        /// Initialize from a deck and/or an eclipse state and (logical cartesian) specified pore volumes.
        GridInit(const EclipseState&, const std::vector<double>&)
        {
            OPM_THROW(std::logic_error, "Found no specialization for GridInit for the requested Grid class.");
        }
    };


    /// Specialization for UnstructuredGrid.
    template <>
    class GridInit<UnstructuredGrid>
    {
    public:
        /// Initialize from a deck and/or an eclipse state and (logical cartesian) specified pore volumes.
        GridInit(const EclipseState& eclipse_state, const std::vector<double>& porv)
            : grid_manager_(eclipse_state.getInputGrid(), porv)
        {
        }
        /// Access the created grid.
        const UnstructuredGrid& grid()
        {
            return *grid_manager_.c_grid();
        }
    private:
        GridManager grid_manager_;
    };


#if HAVE_OPM_GRID
    /// Specialization for PolyhedralGrid.
    template < int dim, int dimworld >
    class GridInit< Dune::PolyhedralGrid< dim, dimworld > >
    {
    public:
        typedef Dune::PolyhedralGrid< dim, dimworld > Grid;
        /// Initialize from a deck and/or an eclipse state and (logical cartesian) specified pore volumes.
        GridInit(const EclipseState& eclipse_state, const std::vector<double>& porv)
            : grid_manager_(eclipse_state.getInputGrid(), porv),
              grid_( *grid_manager_.c_grid() )
        {
        }

        /// Access the created grid.
        const Grid& grid()
        {
            return grid_;
        }
    private:
        GridManager grid_manager_;
        Grid        grid_;
    };


    /// Specialization for CpGrid.
    template <>
    class GridInit<Dune::CpGrid>
    {
    public:
        GridInit()
        {
            gridSelfManaged_ = false;
        }

        /// Initialize from a deck and/or an eclipse state and (logical cartesian) specified pore volumes.
        GridInit(const EclipseState& eclipse_state, const std::vector<double>& porv)
        {
            gridSelfManaged_ = true;

            grid_ = new Dune::CpGrid;
            grid_->processEclipseFormat(eclipse_state.getInputGrid(), false, false, false, porv);
        }

        ~GridInit()
        {
            if (gridSelfManaged_)
                delete grid_;
        }

        /// Access the created grid. Note that mutable access may be required for load balancing.
        Dune::CpGrid& grid()
        {
            return *grid_;
        }

        /// set the grid from the outside
        void setGrid(Dune::CpGrid& newGrid)
        {
            gridSelfManaged_ = false;
            grid_ = &newGrid;
        }

    private:
        Dune::CpGrid* grid_;
        bool gridSelfManaged_;
    };
#endif // HAVE_OPM_GRID


} // namespace Opm

#endif // OPM_GRIDINIT_HEADER_INCLUDED
