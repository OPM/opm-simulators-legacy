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



#include "config.h"
#include <opm/core/props/pvt/PvtPropertiesBasic.hpp>
#include <opm/parser/eclipse/Units/Units.hpp>
#include <opm/common/ErrorMacros.hpp>


namespace Opm
{

    PvtPropertiesBasic::PvtPropertiesBasic()
    {
    }


    void PvtPropertiesBasic::init(const ParameterGroup& param)
    {
        int num_phases = param.getDefault("num_phases", 2);
        if (num_phases > 3 || num_phases < 1) {
            OPM_THROW(std::runtime_error, "PvtPropertiesBasic::init() illegal num_phases: " << num_phases);
        }
        density_.resize(num_phases);
        viscosity_.resize(num_phases);
        // We currently do not allow the user to set B.
        formation_volume_factor_.clear();
        formation_volume_factor_.resize(num_phases, 1.0);

        // Setting mu and rho from parameters
        using namespace Opm::prefix;
        using namespace Opm::unit;
        const double kgpm3 = kilogram/cubic(meter);
        const double cP = centi*Poise;
        std::string rname[3] = { "rho1", "rho2", "rho3" };
        double rdefault[3] = { 1.0e3, 1.0e3, 1.0e3 };
        std::string vname[3] = { "mu1", "mu2", "mu3" };
        double vdefault[3] = { 1.0, 1.0, 1.0 };
        for (int phase = 0; phase < num_phases; ++phase) {
            density_[phase] = kgpm3*param.getDefault(rname[phase], rdefault[phase]);
            viscosity_[phase] = cP*param.getDefault(vname[phase], vdefault[phase]);
        }
    }

    void PvtPropertiesBasic::init(const int num_phases,
                                  const std::vector<double>& rho,
                                  const std::vector<double>& visc)
    {
        if (num_phases > 3 || num_phases < 1) {
            OPM_THROW(std::runtime_error, "PvtPropertiesBasic::init() illegal num_phases: " << num_phases);
        }
        // We currently do not allow the user to set B.
        formation_volume_factor_.clear();
        formation_volume_factor_.resize(num_phases, 1.0);
        density_ = rho;
        viscosity_ = visc;
    }

    const double* PvtPropertiesBasic::surfaceDensities() const
    {
        return &density_[0];
    }


    int PvtPropertiesBasic::numPhases() const
    {
        return density_.size();
    }

    PhaseUsage PvtPropertiesBasic::phaseUsage() const
    {
        PhaseUsage pu;
        pu.num_phases = numPhases();
        if (pu.num_phases == 2) {
            // Might just as well assume water-oil.
            pu.phase_used[BlackoilPhases::Aqua] = true;
            pu.phase_used[BlackoilPhases::Liquid] = true;
            pu.phase_used[BlackoilPhases::Vapour] = false;
            pu.phase_pos[BlackoilPhases::Aqua] = 0;
            pu.phase_pos[BlackoilPhases::Liquid] = 1;
            pu.phase_pos[BlackoilPhases::Vapour] = 1; // Unused.
        } else {
            assert(pu.num_phases == 3);
            pu.phase_used[BlackoilPhases::Aqua] = true;
            pu.phase_used[BlackoilPhases::Liquid] = true;
            pu.phase_used[BlackoilPhases::Vapour] = true;
            pu.phase_pos[BlackoilPhases::Aqua] = 0;
            pu.phase_pos[BlackoilPhases::Liquid] = 1;
            pu.phase_pos[BlackoilPhases::Vapour] = 2;
        }
        return pu;
    }



    void PvtPropertiesBasic::mu(const int n,
                                const double* /*p*/,
                                const double* /*T*/,
                                const double* /*z*/,
                                double* output_mu) const
    {
        const int np = numPhases();
        for (int phase = 0; phase < np; ++phase) {
// #pragma omp parallel for
            for (int i = 0; i < n; ++i) {
                output_mu[np*i + phase] = viscosity_[phase];
            }
        }
    }

    void PvtPropertiesBasic::B(const int n,
                               const double* /*p*/,
                               const double* /*T*/,
                               const double* /*z*/,
                               double* output_B) const
    {
        const int np = numPhases();
        for (int phase = 0; phase < np; ++phase) {
// #pragma omp parallel for
            for (int i = 0; i < n; ++i) {
                output_B[np*i + phase] = formation_volume_factor_[phase];
            }
        }
    }

    void PvtPropertiesBasic::dBdp(const int n,
                                  const double* /*p*/,
                                  const double* /*T*/,
                                  const double* /*z*/,
                                  double* output_B,
                                  double* output_dBdp) const
    {
        const int np = numPhases();
        for (int phase = 0; phase < np; ++phase) {
// #pragma omp parallel for
            for (int i = 0; i < n; ++i) {
                output_B[np*i + phase] = formation_volume_factor_[phase];
                output_dBdp[np*i + phase] = 0.0;
            }
        }

    }


    void PvtPropertiesBasic::R(const int n,
                               const double* /*p*/,
                               const double* /*z*/,
                               double* output_R) const
    {
        const int np = numPhases();
        std::fill(output_R, output_R + n*np, 0.0);
    }

    void PvtPropertiesBasic::dRdp(const int n,
                                  const double* /*p*/,
                                  const double* /*z*/,
                                  double* output_R,
                                  double* output_dRdp) const
    {
        const int np = numPhases();
        std::fill(output_R, output_R + n*np, 0.0);
        std::fill(output_dRdp, output_dRdp + n*np, 0.0);
    }

} // namespace Opm
