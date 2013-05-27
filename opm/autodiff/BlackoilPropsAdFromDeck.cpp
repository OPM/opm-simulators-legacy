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

#include <config.h>

#include <opm/autodiff/BlackoilPropsAdFromDeck.hpp>
#include <opm/autodiff/AutoDiffHelpers.hpp>
#include <opm/core/props/BlackoilPropertiesInterface.hpp>
#include <opm/core/props/BlackoilPhases.hpp>
#include <opm/core/props/pvt/SinglePvtInterface.hpp>
#include <opm/core/props/pvt/SinglePvtConstCompr.hpp>
#include <opm/core/props/pvt/SinglePvtDead.hpp>
#include <opm/core/props/pvt/SinglePvtDeadSpline.hpp>
#include <opm/core/props/pvt/SinglePvtLiveOil.hpp>
#include <opm/core/utility/ErrorMacros.hpp>
#include <opm/core/utility/Units.hpp>

namespace Opm
{

    // Making these typedef to make the code more readable.
    typedef BlackoilPropsAdFromDeck::ADB ADB;
    typedef BlackoilPropsAdFromDeck::V V;
    typedef Eigen::Array<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> Block;
    enum { Aqua = BlackoilPhases::Aqua,
           Liquid = BlackoilPhases::Liquid,
           Vapour = BlackoilPhases::Vapour };

    /// Constructor wrapping an opm-core black oil interface.
    BlackoilPropsAdFromDeck::BlackoilPropsAdFromDeck(const EclipseGridParser& deck,
                                                     const UnstructuredGrid& grid,
                                                     const bool init_rock)
    {
        if (init_rock){
            rock_.init(deck, grid);
        }
        const int samples = 0;


        phase_usage_ = phaseUsageFromDeck(deck);
        // Set the properties.
        props_.resize(phase_usage_.num_phases);
        // Water PVT
        if (phase_usage_.phase_used[Aqua]) {
            if (deck.hasField("PVTW")) {
                props_[phase_usage_.phase_pos[Aqua]].reset(new SinglePvtConstCompr(deck.getPVTW().pvtw_));
            } else {
                // Eclipse 100 default.
                props_[phase_usage_.phase_pos[Aqua]].reset(new SinglePvtConstCompr(0.5*Opm::prefix::centi*Opm::unit::Poise));
            }
        }
        // Oil PVT
        if (phase_usage_.phase_used[Liquid]) {
            if (deck.hasField("PVDO")) {
                if (samples > 0) {
                    props_[phase_usage_.phase_pos[Liquid]].reset(new SinglePvtDeadSpline(deck.getPVDO().pvdo_, samples));
                } else {
                    props_[phase_usage_.phase_pos[Liquid]].reset(new SinglePvtDead(deck.getPVDO().pvdo_));
                }
            } else if (deck.hasField("PVTO")) {

                props_[phase_usage_.phase_pos[Liquid]].reset(new SinglePvtLiveOil(deck.getPVTO().pvto_));
            } else if (deck.hasField("PVCDO")) {
                props_[phase_usage_.phase_pos[Liquid]].reset(new SinglePvtConstCompr(deck.getPVCDO().pvcdo_));
            } else {
                THROW("Input is missing PVDO or PVTO\n");
            }
        }
        // Gas PVT
        if (phase_usage_.phase_used[Vapour]) {
            if (deck.hasField("PVDG")) {
                if (samples > 0) {
                    props_[phase_usage_.phase_pos[Vapour]].reset(new SinglePvtDeadSpline(deck.getPVDG().pvdg_, samples));
                } else {
                    props_[phase_usage_.phase_pos[Vapour]].reset(new SinglePvtDead(deck.getPVDG().pvdg_));
                }
            // } else if (deck.hasField("PVTG")) {
            //     props_[phase_usage_.phase_pos[Vapour]].reset(new SinglePvtLiveGas(deck.getPVTG().pvtg_));
            } else {
                THROW("Input is missing PVDG or PVTG\n");
            }
        }

        SaturationPropsFromDeck<SatFuncGwsegNonuniform>* ptr
            = new SaturationPropsFromDeck<SatFuncGwsegNonuniform>();
        satprops_.reset(ptr);
        ptr->init(deck, grid, -1);

        if (phase_usage_.num_phases != satprops_->numPhases()) {
            THROW("BlackoilPropsAdFromDeck::BlackoilPropsAdFromDeck() - "
                  "Inconsistent number of phases in pvt data (" << phase_usage_.num_phases
                  << ") and saturation-dependent function data (" << satprops_->numPhases() << ").");
        }
    }


    ////////////////////////////
    //      Rock interface    //
    ////////////////////////////

    /// \return   D, the number of spatial dimensions.
    int BlackoilPropsAdFromDeck::numDimensions() const
    {
        return rock_.numDimensions();
    }

    /// \return   N, the number of cells.
    int BlackoilPropsAdFromDeck::numCells() const
    {
        return rock_.numCells();
    }

    /// \return   Array of N porosity values.
    const double* BlackoilPropsAdFromDeck::porosity() const
    {
        return rock_.porosity();
    }

    /// \return   Array of ND^2 permeability values.
    ///           The D^2 permeability values for a cell are organized as a matrix,
    ///           which is symmetric (so ordering does not matter).
    const double* BlackoilPropsAdFromDeck::permeability() const
    {
        return rock_.permeability();
    }


    ////////////////////////////
    //      Fluid interface   //
    ////////////////////////////

    /// \return   Number of active phases (also the number of components).
    int BlackoilPropsAdFromDeck::numPhases() const
    {
        return phase_usage_.num_phases;
    }

    /// \return   Object describing the active phases.
    PhaseUsage BlackoilPropsAdFromDeck::phaseUsage() const
    {
        return phase_usage_;
    }

    // ------ Density ------

    /// Densities of stock components at surface conditions.
    /// \return Array of 3 density values.
    const double* BlackoilPropsAdFromDeck::surfaceDensity() const
    {
        return densities_;
    }


    // ------ Viscosity ------

    /// Water viscosity.
    /// \param[in]  pw     Array of n water pressure values.
    /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
    /// \return            Array of n viscosity values.
    V BlackoilPropsAdFromDeck::muWat(const V& pw,
                                     const Cells& cells) const
    {
        if (!phase_usage_.phase_used[Water]) {
            THROW("Cannot call muWat(): water phase not present.");
        }
        const int n = cells.size();
        ASSERT(pw.size() == n);
        V mu(n);
        V dmudp(n);
        V dmudr(n);
        const double* rs = 0;

        props_[phase_usage_.phase_pos[Water]]->mu(n, pw.data(), rs,
                                                  mu.data(), dmudp.data(), dmudr.data());
        return mu;
    }

    /// Oil viscosity.
    /// \param[in]  po     Array of n oil pressure values.
    /// \param[in]  rs     Array of n gas solution factor values.
    /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
    /// \return            Array of n viscosity values.
    V BlackoilPropsAdFromDeck::muOil(const V& po,
                                     const V& rs,
                                     const Cells& cells) const
    {
        if (!phase_usage_.phase_used[Oil]) {
            THROW("Cannot call muOil(): oil phase not present.");
        }
        const int n = cells.size();
        ASSERT(po.size() == n);
        V mu(n);
        V dmudp(n);
        V dmudr(n);

        props_[phase_usage_.phase_pos[Oil]]->mu(n, po.data(), rs.data(),
                                                mu.data(), dmudp.data(), dmudr.data());
        return mu;
    }

    /// Gas viscosity.
    /// \param[in]  pg     Array of n gas pressure values.
    /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
    /// \return            Array of n viscosity values.
    V BlackoilPropsAdFromDeck::muGas(const V& pg,
                                     const Cells& cells) const
    {
        if (!phase_usage_.phase_used[Gas]) {
            THROW("Cannot call muGas(): gas phase not present.");
        }
        const int n = cells.size();
        ASSERT(pg.size() == n);
        V mu(n);
        V dmudp(n);
        V dmudr(n);
        const double* rs = 0;

        props_[phase_usage_.phase_pos[Gas]]->mu(n, pg.data(), rs,
                                                mu.data(), dmudp.data(), dmudr.data());
        return mu;
    }

    /// Water viscosity.
    /// \param[in]  pw     Array of n water pressure values.
    /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
    /// \return            Array of n viscosity values.
    ADB BlackoilPropsAdFromDeck::muWat(const ADB& pw,
                                       const Cells& cells) const
    {
        if (!phase_usage_.phase_used[Water]) {
            THROW("Cannot call muWat(): water phase not present.");
        }
        const int n = cells.size();
        ASSERT(pw.size() == n);
        V mu(n);
        V dmudp(n);
        V dmudr(n);
        const double* rs = 0;

        props_[phase_usage_.phase_pos[Water]]->mu(n, pw.value().data(), rs,
                                                  mu.data(), dmudp.data(), dmudr.data());
        ADB::M dmudp_diag = spdiag(dmudp);
        const int num_blocks = pw.numBlocks();
        std::vector<ADB::M> jacs(num_blocks);
        for (int block = 0; block < num_blocks; ++block) {
            jacs[block] = dmudp_diag * pw.derivative()[block];
        }
        return ADB::function(mu, jacs);
    }

    /// Oil viscosity.
    /// \param[in]  po     Array of n oil pressure values.
    /// \param[in]  rs     Array of n gas solution factor values.
    /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
    /// \return            Array of n viscosity values.
    ADB BlackoilPropsAdFromDeck::muOil(const ADB& po,
                                       const ADB& rs,
                                       const Cells& cells) const
    {
        if (!phase_usage_.phase_used[Oil]) {
            THROW("Cannot call muOil(): oil phase not present.");
        }
        const int n = cells.size();
        ASSERT(po.size() == n);
        V mu(n);
        V dmudp(n);
        V dmudr(n);

        props_[phase_usage_.phase_pos[Oil]]->mu(n, po.value().data(), rs.value().data(),
                                                mu.data(), dmudp.data(), dmudr.data());

        ADB::M dmudp_diag = spdiag(dmudp);
        ADB::M dmudr_diag = spdiag(dmudr);
        const int num_blocks = po.numBlocks();
        std::vector<ADB::M> jacs(num_blocks);
        for (int block = 0; block < num_blocks; ++block) {
            jacs[block] = dmudp_diag * po.derivative()[block] + dmudr_diag * rs.derivative()[block];
        }
        return ADB::function(mu, jacs);
    }

    /// Gas viscosity.
    /// \param[in]  pg     Array of n gas pressure values.
    /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
    /// \return            Array of n viscosity values.
    ADB BlackoilPropsAdFromDeck::muGas(const ADB& pg,
                                       const Cells& cells) const
    {
        if (!phase_usage_.phase_used[Gas]) {
            THROW("Cannot call muGas(): gas phase not present.");
        }
        const int n = cells.size();
        ASSERT(pg.value().size() == n);
        V mu(n);
        V dmudp(n);
        V dmudr(n);
        const double* rs = 0;

        props_[phase_usage_.phase_pos[Gas]]->mu(n, pg.value().data(), rs,
                                                  mu.data(), dmudp.data(), dmudr.data());

        ADB::M dmudp_diag = spdiag(dmudp);
        const int num_blocks = pg.numBlocks();
        std::vector<ADB::M> jacs(num_blocks);
        for (int block = 0; block < num_blocks; ++block) {
            jacs[block] = dmudp_diag * pg.derivative()[block];
        }
        return ADB::function(mu, jacs);
    }


    // ------ Formation volume factor (b) ------

    // These methods all call the matrix() method, after which the variable
    // (also) called 'matrix' contains, in each row, the A = RB^{-1} matrix for
    // a cell. For three-phase black oil:
    //  A = [  bw       0       0
    //          0       bo      0
    //          0      b0*rs   bw ]
    // Where b = B^{-1}.
    // Therefore, we extract the correct diagonal element, and are done.
    // When we need the derivatives (w.r.t. p, since we don't do w.r.t. rs),
    // we also get the following derivative matrix:
    //  A = [  dbw       0       0
    //          0       dbo      0
    //          0      db0*rs   dbw ]
    // Again, we just extract a diagonal element.

    /// Water formation volume factor.
    /// \param[in]  pw     Array of n water pressure values.
    /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
    /// \return            Array of n formation volume factor values.
    V BlackoilPropsAdFromDeck::bWat(const V& pw,
                                    const Cells& cells) const
    {
        if (!phase_usage_.phase_used[Water]) {
            THROW("Cannot call bWat(): water phase not present.");
        }
        const int n = cells.size();
        ASSERT(pw.size() == n);

        V b(n);
        V dbdp(n);
        V dbdr(n);
        const double* rs = 0;

        props_[phase_usage_.phase_pos[Water]]->b(n, pw.data(), rs,
                                                 b.data(), dbdp.data(), dbdr.data());

        return b;
    }

    /// Oil formation volume factor.
    /// \param[in]  po     Array of n oil pressure values.
    /// \param[in]  rs     Array of n gas solution factor values.
    /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
    /// \return            Array of n formation volume factor values.
    V BlackoilPropsAdFromDeck::bOil(const V& po,
                                    const V& rs,
                                    const Cells& cells) const
    {
        if (!phase_usage_.phase_used[Oil]) {
            THROW("Cannot call bOil(): oil phase not present.");
        }
        const int n = cells.size();
        ASSERT(po.size() == n);

        V b(n);
        V dbdp(n);
        V dbdr(n);

        props_[phase_usage_.phase_pos[Oil]]->b(n, po.data(), rs.data(),
                                               b.data(), dbdp.data(), dbdr.data());

        return b;
    }

    /// Gas formation volume factor.
    /// \param[in]  pg     Array of n gas pressure values.
    /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
    /// \return            Array of n formation volume factor values.
    V BlackoilPropsAdFromDeck::bGas(const V& pg,
                                    const Cells& cells) const
    {
        if (!phase_usage_.phase_used[Gas]) {
            THROW("Cannot call bGas(): gas phase not present.");
        }
        const int n = cells.size();
        ASSERT(pg.size() == n);

        V b(n);
        V dbdp(n);
        V dbdr(n);
        const double* rs = 0;

        props_[phase_usage_.phase_pos[Gas]]->b(n, pg.data(), rs,
                                               b.data(), dbdp.data(), dbdr.data());

        return b;
    }

    /// Water formation volume factor.
    /// \param[in]  pw     Array of n water pressure values.
    /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
    /// \return            Array of n formation volume factor values.
    ADB BlackoilPropsAdFromDeck::bWat(const ADB& pw,
                                      const Cells& cells) const
    {
        if (!phase_usage_.phase_used[Water]) {
            THROW("Cannot call muWat(): water phase not present.");
        }
        const int n = cells.size();
        ASSERT(pw.size() == n);

        V b(n);
        V dbdp(n);
        V dbdr(n);
        const double* rs = 0;

        props_[phase_usage_.phase_pos[Water]]->b(n, pw.value().data(), rs,
                                                 b.data(), dbdp.data(), dbdr.data());

        ADB::M dbdp_diag = spdiag(dbdp);
        const int num_blocks = pw.numBlocks();
        std::vector<ADB::M> jacs(num_blocks);
        for (int block = 0; block < num_blocks; ++block) {
            jacs[block] = dbdp_diag * pw.derivative()[block];
        }
        return ADB::function(b, jacs);
    }

    /// Oil formation volume factor.
    /// \param[in]  po     Array of n oil pressure values.
    /// \param[in]  rs     Array of n gas solution factor values.
    /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
    /// \return            Array of n formation volume factor values.
    ADB BlackoilPropsAdFromDeck::bOil(const ADB& po,
                                      const ADB& rs,
                                      const Cells& cells) const
    {
        if (!phase_usage_.phase_used[Oil]) {
            THROW("Cannot call muOil(): oil phase not present.");
        }
        const int n = cells.size();
        ASSERT(po.size() == n);

        V b(n);
        V dbdp(n);
        V dbdr(n);

        props_[phase_usage_.phase_pos[Oil]]->b(n, po.value().data(), rs.value().data(),
                                               b.data(), dbdp.data(), dbdr.data());

        ADB::M dbdp_diag = spdiag(dbdp);
        ADB::M dbdr_diag = spdiag(dbdr);
        const int num_blocks = po.numBlocks();
        std::vector<ADB::M> jacs(num_blocks);
        for (int block = 0; block < num_blocks; ++block) {
            jacs[block] = dbdp_diag * po.derivative()[block] + dbdr_diag * rs.derivative()[block];
        }
        return ADB::function(b, jacs);
    }

    /// Gas formation volume factor.
    /// \param[in]  pg     Array of n gas pressure values.
    /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
    /// \return            Array of n formation volume factor values.
    ADB BlackoilPropsAdFromDeck::bGas(const ADB& pg,
                                      const Cells& cells) const
    {
        if (!phase_usage_.phase_used[Gas]) {
            THROW("Cannot call muGas(): gas phase not present.");
        }
        const int n = cells.size();
        ASSERT(pg.size() == n);

        V b(n);
        V dbdp(n);
        V dbdr(n);
        const double* rs = 0;

        props_[phase_usage_.phase_pos[Gas]]->b(n, pg.value().data(), rs,
                                               b.data(), dbdp.data(), dbdr.data());

        ADB::M dbdp_diag = spdiag(dbdp);
        const int num_blocks = pg.numBlocks();
        std::vector<ADB::M> jacs(num_blocks);
        for (int block = 0; block < num_blocks; ++block) {
            jacs[block] = dbdp_diag * pg.derivative()[block];
        }
        return ADB::function(b, jacs);
    }



    // ------ Rs bubble point curve ------

    /// Bubble point curve for Rs as function of oil pressure.
    /// \param[in]  po     Array of n oil pressure values.
    /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
    /// \return            Array of n bubble point values for Rs.
    V BlackoilPropsAdFromDeck::rsMax(const V& po,
                                     const Cells& cells) const
    {
        if (!phase_usage_.phase_used[Oil]) {
            THROW("Cannot call rsMax(): oil phase not present.");
        }
        const int n = cells.size();
        ASSERT(po.size() == n);
        V rbub(n);
        V drbubdp(n);
        props_[Oil]->rbub(n, po.data(), rbub.data(), drbubdp.data());
        return rbub;
    }

    /// Bubble point curve for Rs as function of oil pressure.
    /// \param[in]  po     Array of n oil pressure values.
    /// \param[in]  cells  Array of n cell indices to be associated with the pressure values.
    /// \return            Array of n bubble point values for Rs.
    ADB BlackoilPropsAdFromDeck::rsMax(const ADB& po,
                                       const Cells& cells) const
    {
        if (!phase_usage_.phase_used[Oil]) {
            THROW("Cannot call rsMax(): oil phase not present.");
        }
        const int n = cells.size();
        ASSERT(po.size() == n);
        V rbub(n);
        V drbubdp(n);
        props_[Oil]->rbub(n, po.value().data(), rbub.data(), drbubdp.data());
        ADB::M drbubdp_diag = spdiag(drbubdp);
        const int num_blocks = po.numBlocks();
        std::vector<ADB::M> jacs(num_blocks);
        for (int block = 0; block < num_blocks; ++block) {
            jacs[block] = drbubdp_diag * po.derivative()[block];
        }
        return ADB::function(rbub, jacs);
    }

    // ------ Relative permeability ------

    /// Relative permeabilities for all phases.
    /// \param[in]  sw     Array of n water saturation values.
    /// \param[in]  so     Array of n oil saturation values.
    /// \param[in]  sg     Array of n gas saturation values.
    /// \param[in]  cells  Array of n cell indices to be associated with the saturation values.
    /// \return            An std::vector with 3 elements, each an array of n relperm values,
    ///                    containing krw, kro, krg. Use PhaseIndex for indexing into the result.
    std::vector<V> BlackoilPropsAdFromDeck::relperm(const V& sw,
                                                    const V& so,
                                                    const V& sg,
                                                    const Cells& cells) const
    {
        const int n = cells.size();
        const int np = numPhases();
        Block s_all(n, np);
        if (phase_usage_.phase_used[Water]) {
            ASSERT(sw.size() == n);
            s_all.col(phase_usage_.phase_pos[Water]) = sw;
        }
        if (phase_usage_.phase_used[Oil]) {
            ASSERT(so.size() == n);
            s_all.col(phase_usage_.phase_pos[Oil]) = so;
        }
        if (phase_usage_.phase_used[Gas]) {
            ASSERT(sg.size() == n);
            s_all.col(phase_usage_.phase_pos[Gas]) = sg;
        }
        Block kr(n, np);
        satprops_->relperm(n, s_all.data(), cells.data(), kr.data(), 0);
        std::vector<V> relperms;
        relperms.reserve(3);
        for (int phase = 0; phase < 3; ++phase) {
            if (phase_usage_.phase_used[phase]) {
                relperms.emplace_back(kr.col(phase_usage_.phase_pos[phase]));
            } else {
                relperms.emplace_back();
            }
        }
        return relperms;
    }

    /// Relative permeabilities for all phases.
    /// \param[in]  sw     Array of n water saturation values.
    /// \param[in]  so     Array of n oil saturation values.
    /// \param[in]  sg     Array of n gas saturation values.
    /// \param[in]  cells  Array of n cell indices to be associated with the saturation values.
    /// \return            An std::vector with 3 elements, each an array of n relperm values,
    ///                    containing krw, kro, krg. Use PhaseIndex for indexing into the result.
    std::vector<ADB> BlackoilPropsAdFromDeck::relperm(const ADB& sw,
                                                      const ADB& so,
                                                      const ADB& sg,
                                                      const Cells& cells) const
    {
        const int n = cells.size();
        const int np = numPhases();
        Block s_all(n, np);
        if (phase_usage_.phase_used[Water]) {
            ASSERT(sw.value().size() == n);
            s_all.col(phase_usage_.phase_pos[Water]) = sw.value();
        }
        if (phase_usage_.phase_used[Oil]) {
            ASSERT(so.value().size() == n);
            s_all.col(phase_usage_.phase_pos[Oil]) = so.value();
        } else {
            THROW("BlackoilPropsAdFromDeck::relperm() assumes oil phase is active.");
        }
        if (phase_usage_.phase_used[Gas]) {
            ASSERT(sg.value().size() == n);
            s_all.col(phase_usage_.phase_pos[Gas]) = sg.value();
        }
        Block kr(n, np);
        Block dkr(n, np*np);
        satprops_->relperm(n, s_all.data(), cells.data(), kr.data(), dkr.data());
        const int num_blocks = so.numBlocks();
        std::vector<ADB> relperms;
        relperms.reserve(3);
        typedef const ADB* ADBPtr;
        ADBPtr s[3] = { &sw, &so, &sg };
        for (int phase1 = 0; phase1 < 3; ++phase1) {
            if (phase_usage_.phase_used[phase1]) {
                const int phase1_pos = phase_usage_.phase_pos[phase1];
                std::vector<ADB::M> jacs(num_blocks);
                for (int block = 0; block < num_blocks; ++block) {
                    jacs[block] = ADB::M(n, s[phase1]->derivative()[block].cols());
                }
                for (int phase2 = 0; phase2 < 3; ++phase2) {
                    if (!phase_usage_.phase_used[phase2]) {
                        continue;
                    }
                    const int phase2_pos = phase_usage_.phase_pos[phase2];
                    // Assemble dkr1/ds2.
                    const int column = phase1_pos + np*phase2_pos; // Recall: Fortran ordering from props_.relperm()
                    ADB::M dkr1_ds2_diag = spdiag(dkr.col(column));
                    for (int block = 0; block < num_blocks; ++block) {
                        jacs[block] += dkr1_ds2_diag * s[phase2]->derivative()[block];
                    }
                }
                relperms.emplace_back(ADB::function(kr.col(phase1_pos), jacs));
            } else {
                relperms.emplace_back(ADB::null());
            }
        }
        return relperms;
    }

} // namespace Opm

