/*
  Copyright 2013 SINTEF ICT, Applied Mathematics.

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

#ifndef OPM_GEOPROPS_HEADER_INCLUDED
#define OPM_GEOPROPS_HEADER_INCLUDED

#include <opm/core/grid.h>
#include <opm/autodiff/GridHelpers.hpp>
#include <opm/common/ErrorMacros.hpp>
//#include <opm/core/pressure/tpfa/trans_tpfa.h>
#include <opm/core/pressure/tpfa/TransTpfa.hpp>

#include <opm/parser/eclipse/EclipseState/EclipseState.hpp>
#include <opm/parser/eclipse/EclipseState/Grid/EclipseGrid.hpp>
#include <opm/parser/eclipse/EclipseState/Grid/GridProperty.hpp>
#include <opm/parser/eclipse/EclipseState/Grid/NNC.hpp>
#include <opm/parser/eclipse/EclipseState/Grid/TransMult.hpp>
#include <opm/core/grid/PinchProcessor.hpp>
#include <opm/common/utility/platform_dependent/disable_warnings.h>
#include <opm/output/Cells.hpp>

#include <Eigen/Eigen>

#ifdef HAVE_OPM_GRID
#include <dune/common/version.hh>
#include <dune/grid/CpGrid.hpp>
#include <dune/grid/common/mcmgmapper.hh>
#endif

#include <opm/common/utility/platform_dependent/reenable_warnings.h>

#include <cstddef>

namespace Opm
{

    /// Class containing static geological properties that are
    /// derived from grid and petrophysical properties:
    ///   - pore volume
    ///   - transmissibilities
    ///   - gravity potentials
    class DerivedGeology
    {
    public:
        typedef Eigen::ArrayXd Vector;

        /// Construct contained derived geological properties
        /// from grid and property information.
        template <class Props, class Grid>
        DerivedGeology(const Grid&              grid,
                       const Props&             props ,
                       Opm::EclipseStateConstPtr eclState,
                       const bool               use_local_perm,
                       const double*            grav = 0

                )
            : pvol_ (Opm::AutoDiffGrid::numCells(grid))
            , trans_(Opm::AutoDiffGrid::numFaces(grid))
            , gpot_ (Vector::Zero(Opm::AutoDiffGrid::cell2Faces(grid).noEntries(), 1))
            , z_(Opm::AutoDiffGrid::numCells(grid))
            , use_local_perm_(use_local_perm)
        {
            update(grid, props, eclState, grav);
        }

        /// compute the all geological properties at a given report step
        template <class Props, class Grid>
        void update(const Grid&              grid,
                    const Props&             props ,
                    Opm::EclipseStateConstPtr eclState,
                    const double*            grav)

        {
            int numCells = AutoDiffGrid::numCells(grid);
            int numFaces = AutoDiffGrid::numFaces(grid);
            const int *cartDims = AutoDiffGrid::cartDims(grid);
            int numCartesianCells =
                cartDims[0]
                * cartDims[1]
                * cartDims[2];

            // get the pore volume multipliers from the EclipseState
            std::vector<double> multpv(numCartesianCells, 1.0);
            const auto& eclProps = eclState->get3DProperties();
            if (eclProps.hasDeckDoubleGridProperty("MULTPV")) {
                multpv = eclProps.getDoubleGridProperty("MULTPV").getData();
            }

            // get the net-to-gross cell thickness from the EclipseState
            std::vector<double> ntg(numCartesianCells, 1.0);
            if (eclProps.hasDeckDoubleGridProperty("NTG")) {
                ntg = eclProps.getDoubleGridProperty("NTG").getData();
            }

            // Get grid from parser.
            EclipseGridConstPtr eclgrid = eclState->getInputGrid();

            // Pore volume.
            // New keywords MINPVF will add some PV due to OPM cpgrid process algorithm.
            // But the default behavior is to get the comparable pore volume with ECLIPSE.
            for (int cellIdx = 0; cellIdx < numCells; ++cellIdx) {
                int cartesianCellIdx = AutoDiffGrid::globalCell(grid)[cellIdx];
                pvol_[cellIdx] =
                    props.porosity()[cellIdx]
                    * multpv[cartesianCellIdx]
                    * ntg[cartesianCellIdx];
                if (eclgrid->getMinpvMode() == MinpvMode::ModeEnum::OpmFIL) {
                    pvol_[cellIdx] *= AutoDiffGrid::cellVolume(grid, cellIdx);
                } else {
                    pvol_[cellIdx] *= eclgrid->getCellVolume(cartesianCellIdx);
                }
            }

            // Non-neighbour connections.
            nnc_ = eclState->getInputNNC();

            // Transmissibility
            Vector htrans(AutoDiffGrid::numCellFaces(grid));
            Grid* ug = const_cast<Grid*>(& grid);

            if (! use_local_perm_) {
                tpfa_htrans_compute(ug, props.permeability(), htrans.data());
            }
            else {
                tpfa_loc_trans_compute_(grid,eclgrid, props.permeability(),htrans);
            }

            // Use volume weighted arithmetic average of the NTG values for
            // the cells effected by the current OPM cpgrid process algorithm
            // for MINPV. Note that the change does not effect the pore volume calculations
            // as the pore volume is currently defaulted to be comparable to ECLIPSE, but
            // only the transmissibility calculations.
            bool opmfil = eclgrid->getMinpvMode() == MinpvMode::ModeEnum::OpmFIL;
            // opmfil is hardcoded to be true. i.e the volume weighting is always used
            opmfil = true;
            if (opmfil) {
                minPvFillProps_(grid, eclState, ntg);
            }

            std::vector<double> mult;
            multiplyHalfIntersections_(grid, eclState, ntg, htrans, mult);

            if (!opmfil && eclgrid->isPinchActive()) {
                // opmfil is hardcoded to be true. i.e the pinch processor is never used
                pinchProcess_(grid, *eclState, htrans, numCells);
            }

            // combine the half-face transmissibilites into the final face
            // transmissibilites.
            tpfa_trans_compute(ug, htrans.data(), trans_.data());

            // multiply the face transmissibilities with their appropriate
            // transmissibility multipliers
            for (int faceIdx = 0; faceIdx < numFaces; faceIdx++) {
                trans_[faceIdx] *= mult[faceIdx];
            }

            // Create the set of noncartesian connections.
            noncartesian_ = nnc_;
            exportNncStructure(grid);

            // Compute z coordinates
            for (int c = 0; c<numCells; ++c){
                z_[c] = Opm::UgGridHelpers::cellCenterDepth(grid, c);
            }


            // Gravity potential
            std::fill(gravity_, gravity_ + 3, 0.0);
            if (grav != 0) {
                const typename Vector::Index nd = AutoDiffGrid::dimensions(grid);
                typedef typename AutoDiffGrid::ADCell2FacesTraits<Grid>::Type Cell2Faces;
                Cell2Faces c2f=AutoDiffGrid::cell2Faces(grid);

                std::size_t i = 0;
                for (typename Vector::Index c = 0; c < numCells; ++c) {
                    const double* const cc = AutoDiffGrid::cellCentroid(grid, c);

                    typename Cell2Faces::row_type faces=c2f[c];
                    typedef typename Cell2Faces::row_type::iterator Iter;

                    for (Iter f=faces.begin(), end=faces.end(); f!=end; ++f, ++i) {
                        auto fc = AutoDiffGrid::faceCentroid(grid, *f);

                        for (typename Vector::Index d = 0; d < nd; ++d) {
                            gpot_[i] += grav[d] * (fc[d] - cc[d]);
                        }
                    }
                }
                std::copy(grav, grav + nd, gravity_);
            }
        }




        const Vector& poreVolume()       const { return pvol_   ;}
        const Vector& transmissibility() const { return trans_  ;}
        const Vector& gravityPotential() const { return gpot_   ;}
        const Vector& z()                const { return z_      ;}
        const double* gravity()          const { return gravity_;}
        Vector&       poreVolume()             { return pvol_   ;}
        Vector&       transmissibility()       { return trans_  ;}
        const NNC& nnc() const { return nnc_;}
        const NNC& nonCartesianConnections() const { return noncartesian_;}


        /// Most properties are loaded by the parser, and managed by
        /// the EclipseState class in the opm-parser. However - some
        /// properties must be calculated by the simulator, the
        /// purpose of this method is to calculate these properties in
        /// a form suitable for output. Currently the transmissibility
        /// is the only property calculated this way:
        ///
        /// The grid properties TRANX, TRANY and TRANZ are initialized
        /// in a form suitable for writing to the INIT file. These
        /// properties should be interpreted with a
        /// 'the-grid-is-nearly-cartesian' mindset:
        ///
        ///   TRANX[i,j,k] = T on face between cells (i,j,k) and (i+1,j  ,k  )
        ///   TRANY[i,j,k] = T on face between cells (i,j,k) and (i  ,j+1,k  )
        ///   TRANZ[i,j,k] = T on face between cells (i,j,k) and (i  ,j  ,k+1)
        ///
        /// If the grid structure has no resemblance to a cartesian
        /// grid the whole TRAN keyword is quite meaningless.

        template <class Grid>
        const std::vector<data::CellData> simProps( const Grid& grid ) const {
            using namespace UgGridHelpers;
            const int* dims = cartDims( grid );
            const int globalSize = dims[0] * dims[1] * dims[2];
            const auto& trans = this->transmissibility( );

            data::CellData tranx = {"TRANX" , UnitSystem::measure::transmissibility, std::vector<double>( globalSize )};
            data::CellData trany = {"TRANY" , UnitSystem::measure::transmissibility, std::vector<double>( globalSize )};
            data::CellData tranz = {"TRANZ" , UnitSystem::measure::transmissibility, std::vector<double>( globalSize )};

            size_t num_faces = numFaces(grid);
            auto fc = faceCells(grid);
            for (size_t i = 0; i < num_faces; ++i) {
                auto c1 = std::min( fc(i,0) , fc(i,1));
                auto c2 = std::max( fc(i,0) , fc(i,1));

                if (c1 == -1 || c2 == -1)
                    continue;

                c1 = globalCell(grid) ? globalCell(grid)[c1] : c1;
                c2 = globalCell(grid) ? globalCell(grid)[c2] : c2;

                if ((c2 - c1) == 1) {
                    tranx.data[c1] = trans[i];
                }

                if ((c2 - c1) == dims[0]) {
                    trany.data[c1] = trans[i];
                }

                if ((c2 - c1) == dims[0]*dims[1]) {
                    tranz.data[c1] = trans[i];
                }
            }

            std::vector<data::CellData> tran;
            tran.push_back( std::move( tranx ));
            tran.push_back( std::move( trany ));
            tran.push_back( std::move( tranz ));

            return tran;
        }


    private:
        template <class Grid>
        void multiplyHalfIntersections_(const Grid &grid,
                                        Opm::EclipseStateConstPtr eclState,
                                        const std::vector<double> &ntg,
                                        Vector &halfIntersectTransmissibility,
                                        std::vector<double> &intersectionTransMult);

        template <class Grid>
        void tpfa_loc_trans_compute_(const Grid &grid,
                                     Opm::EclipseGridConstPtr eclGrid,
                                     const double* perm,
                                     Vector &hTrans);

        template <class Grid>
        void minPvFillProps_(const Grid &grid,
                             Opm::EclipseStateConstPtr eclState,
                             std::vector<double> &ntg);

        template <class Grid>
        void pinchProcess_(const Grid& grid,
                           const Opm::EclipseState& eclState,
                           const Vector& htrans,
                                 int numCells);


        /// checks cartesian adjacency of global indices g1 and g2
        template <typename Grid>
        bool cartesianAdjacent(const Grid& grid, int g1, int g2) {
            int diff = std::abs(g1 - g2);

            const int * dimens = UgGridHelpers::cartDims(grid);
            if (diff == 1)
               return true;
            if (diff == dimens[0])
               return true;
            if (diff == dimens[0] * dimens[1])
               return true;

            return false;
        }


        /// Write the NNC structure of the given grid to NNC.
        ///
        /// Write cell adjacencies beyond Cartesian neighborhoods to NNC.
        ///
        /// The trans vector is indexed by face number as it is in grid.
        template <typename Grid>
        void exportNncStructure(const Grid& grid) {
            // we use numFaces, numCells, cell2Faces, globalCell from UgGridHelpers
            using namespace UgGridHelpers;

            size_t num_faces = numFaces(grid);

            auto fc = faceCells(grid);
            for (size_t i = 0; i < num_faces; ++i) {
                auto c1 = fc(i, 0);
                auto c2 = fc(i, 1);

                if (c1 == -1 || c2 == -1)
                    continue; // face on grid boundary
                // translate from active cell idx (ac1,ac2) to global cell idx
                c1 = globalCell(grid) ? globalCell(grid)[c1] : c1;
                c2 = globalCell(grid) ? globalCell(grid)[c2] : c2;
                if (!cartesianAdjacent(grid, c1, c2)) {
                    // suppose c1,c2 is specified in ECLIPSE input
                    // we here overwrite its trans by grid's
                    noncartesian_.addNNC(c1, c2, trans_[i]);
                }
            }
        }

        Vector pvol_ ;
        Vector trans_;
        Vector gpot_ ;
        Vector z_;
        double gravity_[3]; // Size 3 even if grid is 2-dim.
        bool use_local_perm_;

        // Non-neighboring connections
        NNC nnc_;
        // Non-cartesian connections
        NNC noncartesian_;
    };

    template <class GridType>
    inline void DerivedGeology::minPvFillProps_(const GridType &grid,
                                                Opm::EclipseStateConstPtr eclState,
                                                std::vector<double> &ntg)
    {

        int numCells = Opm::AutoDiffGrid::numCells(grid);
        const int* global_cell = Opm::UgGridHelpers::globalCell(grid);
        const int* cartdims = Opm::UgGridHelpers::cartDims(grid);
        EclipseGridConstPtr eclgrid = eclState->getInputGrid();
        const auto& porv = eclState->get3DProperties().getDoubleGridProperty("PORV").getData();
        const auto& actnum = eclState->get3DProperties().getIntGridProperty("ACTNUM").getData();
        for (int cellIdx = 0; cellIdx < numCells; ++cellIdx) {
            const int nx = cartdims[0];
            const int ny = cartdims[1];
            const int cartesianCellIdx = global_cell[cellIdx];

            const double cellVolume = eclgrid->getCellVolume(cartesianCellIdx);
            ntg[cartesianCellIdx] *= cellVolume;
            double totalCellVolume = cellVolume;

            // Average properties as long as there exist cells above
            // that has pore volume less than the MINPV threshold
            int cartesianCellIdxAbove = cartesianCellIdx - nx*ny;
            while ( cartesianCellIdxAbove >= 0 &&
                 actnum[cartesianCellIdxAbove] > 0 &&
                 porv[cartesianCellIdxAbove] < eclgrid->getMinpvValue() ) {

                // Volume weighted arithmetic average of NTG
                const double cellAboveVolume = eclgrid->getCellVolume(cartesianCellIdxAbove);
                totalCellVolume += cellAboveVolume;
                ntg[cartesianCellIdx] += ntg[cartesianCellIdxAbove]*cellAboveVolume;
                cartesianCellIdxAbove -= nx*ny;
            }
            ntg[cartesianCellIdx] /= totalCellVolume;
        }
    }




    template <class GridType>
       inline void DerivedGeology::pinchProcess_(const GridType& grid,
                                                 const Opm::EclipseState& eclState,
                                                 const Vector& htrans,
                                                       int numCells)
       {
        // NOTE that this function is currently never invoked due to
        // opmfil being hardcoded to be true.
        auto  eclgrid = eclState.getInputGrid();
        auto& eclProps = eclState.get3DProperties();
        const double minpv = eclgrid->getMinpvValue();
        const double thickness = eclgrid->getPinchThresholdThickness();
        auto transMode = eclgrid->getPinchOption();
        auto multzMode = eclgrid->getMultzOption();
        PinchProcessor<GridType> pinch(minpv, thickness, transMode, multzMode);

        std::vector<double> htrans_copy(htrans.size());
        std::copy_n(htrans.data(), htrans.size(), htrans_copy.begin());

        std::vector<int> actnum;
        eclgrid->exportACTNUM(actnum);

        const auto& transMult = eclState.getTransMult();
        std::vector<double> multz(numCells, 0.0);
        const int* global_cell = Opm::UgGridHelpers::globalCell(grid);

        for (int i = 0; i < numCells; ++i) {
            multz[i] = transMult.getMultiplier(global_cell[i], Opm::FaceDir::ZPlus);
        }

        // Note the pore volume from eclState is used and not the pvol_ calculated above
        const auto& porv = eclProps.getDoubleGridProperty("PORV").getData();
        pinch.process(grid, htrans_copy, actnum, multz, porv, nnc_);
    }




    template <class GridType>
    inline void DerivedGeology::multiplyHalfIntersections_(const GridType &grid,
                                                           Opm::EclipseStateConstPtr eclState,
                                                           const std::vector<double> &ntg,
                                                           Vector &halfIntersectTransmissibility,
                                                           std::vector<double> &intersectionTransMult)
    {
        int numCells = Opm::AutoDiffGrid::numCells(grid);

        int numIntersections = Opm::AutoDiffGrid::numFaces(grid);
        intersectionTransMult.resize(numIntersections);
        std::fill(intersectionTransMult.begin(), intersectionTransMult.end(), 1.0);

        const TransMult& multipliers = eclState->getTransMult();
        auto cell2Faces = Opm::UgGridHelpers::cell2Faces(grid);
        auto faceCells  = Opm::AutoDiffGrid::faceCells(grid);
        const int* global_cell = Opm::UgGridHelpers::globalCell(grid);
        int cellFaceIdx = 0;

        for (int cellIdx = 0; cellIdx < numCells; ++cellIdx) {
            // loop over all logically-Cartesian faces of the current cell
            auto cellFacesRange = cell2Faces[cellIdx];

            for(auto cellFaceIter = cellFacesRange.begin(), cellFaceEnd = cellFacesRange.end();
                cellFaceIter != cellFaceEnd; ++cellFaceIter, ++cellFaceIdx)
            {
                // the index of the current cell in arrays for the logically-Cartesian grid
                int cartesianCellIdx = global_cell[cellIdx];

                // The index of the face in the compressed grid
                int faceIdx = *cellFaceIter;

                // the logically-Cartesian direction of the face
                int faceTag = Opm::UgGridHelpers::faceTag(grid, cellFaceIter);

                // Translate the C face tag into the enum used by opm-parser's TransMult class
                Opm::FaceDir::DirEnum faceDirection;
                if (faceTag == 0) // left
                    faceDirection = Opm::FaceDir::XMinus;
                else if (faceTag == 1) // right
                    faceDirection = Opm::FaceDir::XPlus;
                else if (faceTag == 2) // back
                    faceDirection = Opm::FaceDir::YMinus;
                else if (faceTag == 3) // front
                    faceDirection = Opm::FaceDir::YPlus;
                else if (faceTag == 4) // bottom
                    faceDirection = Opm::FaceDir::ZMinus;
                else if (faceTag == 5) // top
                    faceDirection = Opm::FaceDir::ZPlus;
                else
                    OPM_THROW(std::logic_error, "Unhandled face direction: " << faceTag);

                // Account for NTG in horizontal one-sided transmissibilities
                switch (faceDirection) {
                case Opm::FaceDir::XMinus:
                case Opm::FaceDir::XPlus:
                case Opm::FaceDir::YMinus:
                case Opm::FaceDir::YPlus:
                    halfIntersectTransmissibility[cellFaceIdx] *= ntg[cartesianCellIdx];
                    break;
                default:
                    // do nothing for the top and bottom faces
                    break;
                }

                // Multiplier contribution on this face for MULT[XYZ] logical cartesian multipliers
                intersectionTransMult[faceIdx] *=
                    multipliers.getMultiplier(cartesianCellIdx, faceDirection);

                // Multiplier contribution on this fase for region multipliers
                const int cellIdxInside  = faceCells(faceIdx, 0);
                const int cellIdxOutside = faceCells(faceIdx, 1);

                // Do not apply region multipliers in the case of boundary connections
                if (cellIdxInside < 0 || cellIdxOutside < 0) {
                    continue;
                }
                const int cartesianCellIdxInside = global_cell[cellIdxInside];
                const int cartesianCellIdxOutside = global_cell[cellIdxOutside];
                //  Only apply the region multipliers from the inside
                if (cartesianCellIdx == cartesianCellIdxInside) {
                    intersectionTransMult[faceIdx] *= multipliers.getRegionMultiplier(cartesianCellIdxInside,cartesianCellIdxOutside,faceDirection);
                }


            }
        }
    }

    template <class GridType>
    inline void DerivedGeology::tpfa_loc_trans_compute_(const GridType& grid,
                                                        Opm::EclipseGridConstPtr eclGrid,
                                                        const double* perm,
                                                        Vector& hTrans){

        // Using Local coordinate system for the transmissibility calculations
        // hTrans(cellFaceIdx) = K(cellNo,j) * sum( C(:,i) .* N(:,j), 2) / sum(C.*C, 2)
        // where K is a diagonal permeability tensor, C is the distance from cell centroid
        // to face centroid and N is the normal vector  pointing outwards with norm equal to the face area.
        // Off-diagonal permeability values are ignored without warning
        int numCells = AutoDiffGrid::numCells(grid);
        int cellFaceIdx = 0;
        auto cell2Faces = Opm::UgGridHelpers::cell2Faces(grid);
        auto faceCells = Opm::UgGridHelpers::faceCells(grid);

        for (int cellIdx = 0; cellIdx < numCells; ++cellIdx) {
            // loop over all logically-Cartesian faces of the current cell
            auto cellFacesRange = cell2Faces[cellIdx];

            for(auto cellFaceIter = cellFacesRange.begin(), cellFaceEnd = cellFacesRange.end();
                cellFaceIter != cellFaceEnd; ++cellFaceIter, ++cellFaceIdx)
            {
                // The index of the face in the compressed grid
                const int faceIdx = *cellFaceIter;

                // the logically-Cartesian direction of the face
                const int faceTag = Opm::UgGridHelpers::faceTag(grid, cellFaceIter);

                // d = 0: XPERM d = 4: YPERM d = 8: ZPERM ignores off-diagonal permeability values.
                const int d = std::floor(faceTag/2) * 4;

                // compute the half transmissibility
                double dist = 0.0;
                double cn = 0.0;
                double sgn = 2.0 * (faceCells(faceIdx, 0) == cellIdx) - 1;
                const int dim = Opm::UgGridHelpers::dimensions(grid);

                const double* faceNormal = Opm::UgGridHelpers::faceNormal(grid, faceIdx);
#if HAVE_OPM_GRID
                assert( dim <= 3 );
                Dune::FieldVector< double, 3 > scaledFaceNormal( 0 );
                for (int indx = 0; indx < dim; ++indx) {
                    scaledFaceNormal[ indx ] = faceNormal[ indx ];
                }
                // compute unit normal incase the normal is already scaled
                scaledFaceNormal /= scaledFaceNormal.two_norm();
                // compute proper normal scaled with face area
                scaledFaceNormal *= Opm::UgGridHelpers::faceArea(grid, faceIdx);
#else
                const double* scaledFaceNormal = faceNormal;
#endif

                int cartesianCellIdx = AutoDiffGrid::globalCell(grid)[cellIdx];
                auto cellCenter = eclGrid->getCellCenter(cartesianCellIdx);
                for (int indx = 0; indx < dim; ++indx) {
                    const double Ci = Opm::UgGridHelpers::faceCentroid(grid, faceIdx)[indx] - cellCenter[indx];
                    dist += Ci*Ci;
                    cn += sgn * Ci * scaledFaceNormal[ indx ];
                }

                if (cn < 0){
                    switch (d) {
                    case 0:
                        OPM_MESSAGE("Warning: negative X-transmissibility value in cell: " << cellIdx << " replace by absolute value") ;
                                break;
                    case 4:
                        OPM_MESSAGE("Warning: negative Y-transmissibility value in cell: " << cellIdx << " replace by absolute value") ;
                                break;
                    case 8:
                        OPM_MESSAGE("Warning: negative Z-transmissibility value in cell: " << cellIdx << " replace by absolute value") ;
                                break;
                    default:
                        OPM_THROW(std::logic_error, "Inconsistency in the faceTag in cell: " << cellIdx);

                    }
                    cn = -cn;
                }
                hTrans[cellFaceIdx] = perm[cellIdx*dim*dim + d] * cn / dist;
            }
        }

    }

}



#endif // OPM_GEOPROPS_HEADER_INCLUDED
