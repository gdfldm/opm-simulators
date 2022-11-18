/*
  Copyright 2017 SINTEF Digital, Mathematics and Cybernetics.
  Copyright 2017 Statoil ASA.
  Copyright 2016 - 2017 IRIS AS.

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
#include <opm/simulators/wells/MultisegmentWellAssemble.hpp>

#include <opm/core/props/BlackoilPhases.hpp>

#include <opm/material/fluidsystems/BlackOilFluidSystem.hpp>

#include <opm/models/blackoil/blackoilindices.hh>
#include <opm/models/blackoil/blackoilonephaseindices.hh>
#include <opm/models/blackoil/blackoiltwophaseindices.hh>

#include <opm/simulators/wells/MultisegmentWellEquations.hpp>
#include <opm/simulators/wells/WellAssemble.hpp>
#include <opm/simulators/wells/WellBhpThpCalculator.hpp>
#include <opm/simulators/wells/WellInterfaceIndices.hpp>

namespace Opm {

template<class FluidSystem, class Indices, class Scalar>
void MultisegmentWellAssemble<FluidSystem,Indices,Scalar>::
assembleControlEq(const WellState& well_state,
                  const GroupState& group_state,
                  const Schedule& schedule,
                  const SummaryState& summaryState,
                  const Well::InjectionControls& inj_controls,
                  const Well::ProductionControls& prod_controls,
                  const double rho,
                  const EvalWell& wqTotal,
                  const EvalWell& bhp,
                  const std::function<EvalWell(const int)>& getQs,
                  Equations& eqns,
                  DeferredLogger& deferred_logger) const
{
    static constexpr int Gas = BlackoilPhases::Vapour;
    static constexpr int Oil = BlackoilPhases::Liquid;
    static constexpr int Water = BlackoilPhases::Aqua;

    EvalWell control_eq(0.0);

    const auto& well = well_.wellEcl();

    auto getRates = [&]() {
        std::vector<EvalWell> rates(3, 0.0);
        if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
            rates[Water] = getQs(Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx));
        }
        if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx)) {
            rates[Oil] = getQs(Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx));
        }
        if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
            rates[Gas] = getQs(Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx));
        }
        return rates;
    };

    if (well_.wellIsStopped()) {
        control_eq = wqTotal;
    } else if (well_.isInjector() ) {
        // Find scaling factor to get injection rate,
        const InjectorType injectorType = inj_controls.injector_type;
        double scaling = 1.0;
        const auto& pu = well_.phaseUsage();
        switch (injectorType) {
        case InjectorType::WATER:
        {
            scaling = well_.scalingFactor(pu.phase_pos[BlackoilPhases::Aqua]);
            break;
        }
        case InjectorType::OIL:
        {
            scaling = well_.scalingFactor(pu.phase_pos[BlackoilPhases::Liquid]);
            break;
        }
        case InjectorType::GAS:
        {
            scaling = well_.scalingFactor(pu.phase_pos[BlackoilPhases::Vapour]);
            break;
        }
        default:
            throw("Expected WATER, OIL or GAS as type for injectors " + well.name());
        }
        const EvalWell injection_rate = wqTotal / scaling;
        // Setup function for evaluation of BHP from THP (used only if needed).
        std::function<EvalWell()> bhp_from_thp = [&]() {
            const auto rates = getRates();
            return WellBhpThpCalculator(well_).calculateBhpFromThp(well_state,
                                                                   rates,
                                                                   well,
                                                                   summaryState,
                                                                   rho,
                                                                   deferred_logger);
        };
        // Call generic implementation.
        WellAssemble(well_).assembleControlEqInj(well_state,
                                                 group_state,
                                                 schedule,
                                                 summaryState,
                                                 inj_controls,
                                                 bhp,
                                                 injection_rate,
                                                 bhp_from_thp,
                                                 control_eq,
                                                 deferred_logger);
    } else {
        // Find rates.
        const auto rates = getRates();
        // Setup function for evaluation of BHP from THP (used only if needed).
        std::function<EvalWell()> bhp_from_thp = [&]() {
            return WellBhpThpCalculator(well_).calculateBhpFromThp(well_state,
                                                                   rates,
                                                                   well,
                                                                   summaryState,
                                                                   rho,
                                                                   deferred_logger);
        };
        // Call generic implementation.
        WellAssemble(well_).assembleControlEqProd(well_state,
                                                  group_state,
                                                  schedule,
                                                  summaryState,
                                                  prod_controls,
                                                  bhp,
                                                  rates,
                                                  bhp_from_thp,
                                                  control_eq,
                                                  deferred_logger);
    }

    // using control_eq to update the matrix and residuals
    eqns.resWell_[0][SPres] = control_eq.value();
    for (int pv_idx = 0; pv_idx < numWellEq; ++pv_idx) {
        eqns.duneD_[0][0][SPres][pv_idx] = control_eq.derivative(pv_idx + Indices::numEq);
    }
}

template<class FluidSystem, class Indices, class Scalar>
void MultisegmentWellAssemble<FluidSystem,Indices,Scalar>::
assemblePressureLoss(const int seg,
                     const int seg_upwind,
                     const EvalWell& accelerationPressureLoss,
                     Equations& eqns) const
{
    eqns.resWell_[seg][SPres] -= accelerationPressureLoss.value();
    eqns.duneD_[seg][seg][SPres][SPres] -= accelerationPressureLoss.derivative(SPres + Indices::numEq);
    eqns.duneD_[seg][seg][SPres][WQTotal] -= accelerationPressureLoss.derivative(WQTotal + Indices::numEq);
    if constexpr (has_wfrac_variable) {
        eqns.duneD_[seg][seg_upwind][SPres][WFrac] -= accelerationPressureLoss.derivative(WFrac + Indices::numEq);
    }
    if constexpr (has_gfrac_variable) {
        eqns.duneD_[seg][seg_upwind][SPres][GFrac] -= accelerationPressureLoss.derivative(GFrac + Indices::numEq);
    }
}

template<class FluidSystem, class Indices, class Scalar>
void MultisegmentWellAssemble<FluidSystem,Indices,Scalar>::
assemblePressureEq(const int seg,
                   const int seg_upwind,
                   const int outlet_segment_index,
                   const EvalWell& pressure_equation,
                   const EvalWell& outlet_pressure,
                   Equations& eqns,
                   bool wfrac,
                   bool gfrac) const
{
    eqns.resWell_[seg][SPres] = pressure_equation.value();
    eqns.duneD_[seg][seg][SPres][SPres] += pressure_equation.derivative(SPres + Indices::numEq);
    eqns.duneD_[seg][seg][SPres][WQTotal] += pressure_equation.derivative(WQTotal + Indices::numEq);
    if (wfrac) {
        eqns.duneD_[seg][seg_upwind][SPres][WFrac] += pressure_equation.derivative(WFrac + Indices::numEq);
    }
    if (gfrac) {
        eqns.duneD_[seg][seg_upwind][SPres][GFrac] += pressure_equation.derivative(GFrac + Indices::numEq);
    }

    // contribution from the outlet segment
    eqns.resWell_[seg][SPres] -= outlet_pressure.value();
    for (int pv_idx = 0; pv_idx < numWellEq; ++pv_idx) {
        eqns.duneD_[seg][outlet_segment_index][SPres][pv_idx] = -outlet_pressure.derivative(pv_idx + Indices::numEq);
    }
}

template<class FluidSystem, class Indices, class Scalar>
void MultisegmentWellAssemble<FluidSystem,Indices,Scalar>::
assembleTrivialEq(const int seg,
                  const Scalar value,
                  Equations& eqns) const
{
    eqns.resWell_[seg][SPres] = value;
    eqns.duneD_[seg][seg][SPres][WQTotal] = 1.;
}

template<class FluidSystem, class Indices, class Scalar>
void MultisegmentWellAssemble<FluidSystem,Indices,Scalar>::
assembleAccumulationTerm(const int seg,
                         const int comp_idx,
                         const EvalWell& accumulation_term,
                         Equations& eqns) const
{
    eqns.resWell_[seg][comp_idx] += accumulation_term.value();
    for (int pv_idx = 0; pv_idx < numWellEq; ++pv_idx) {
      eqns.duneD_[seg][seg][comp_idx][pv_idx] += accumulation_term.derivative(pv_idx + Indices::numEq);
    }
}

template<class FluidSystem, class Indices, class Scalar>
void MultisegmentWellAssemble<FluidSystem,Indices,Scalar>::
assembleOutflowTerm(const int seg,
                    const int seg_upwind,
                    const int comp_idx,
                    const EvalWell& segment_rate,
                    Equations& eqns) const
{
    eqns.resWell_[seg][comp_idx] -= segment_rate.value();
    eqns.duneD_[seg][seg][comp_idx][WQTotal] -= segment_rate.derivative(WQTotal + Indices::numEq);
    if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
        eqns.duneD_[seg][seg_upwind][comp_idx][WFrac] -= segment_rate.derivative(WFrac + Indices::numEq);
    }
    if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
        eqns.duneD_[seg][seg_upwind][comp_idx][GFrac] -= segment_rate.derivative(GFrac + Indices::numEq);
    }
    // pressure derivative should be zero
}

template<class FluidSystem, class Indices, class Scalar>
void MultisegmentWellAssemble<FluidSystem,Indices,Scalar>::
assembleInflowTerm(const int seg,
                   const int inlet,
                   const int inlet_upwind,
                   const int comp_idx,
                   const EvalWell& inlet_rate,
                   Equations& eqns) const
 {
    eqns.resWell_[seg][comp_idx] += inlet_rate.value();
    eqns.duneD_[seg][inlet][comp_idx][WQTotal] += inlet_rate.derivative(WQTotal + Indices::numEq);
    if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
        eqns.duneD_[seg][inlet_upwind][comp_idx][WFrac] += inlet_rate.derivative(WFrac + Indices::numEq);
    }
    if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
        eqns.duneD_[seg][inlet_upwind][comp_idx][GFrac] += inlet_rate.derivative(GFrac + Indices::numEq);
    }
    // pressure derivative should be zero
}

#define INSTANCE(...) \
template class MultisegmentWellAssemble<BlackOilFluidSystem<double,BlackOilDefaultIndexTraits>,__VA_ARGS__,double>;

// One phase
INSTANCE(BlackOilOnePhaseIndices<0u,0u,0u,0u,false,false,0u,1u,0u>)
INSTANCE(BlackOilOnePhaseIndices<0u,0u,0u,1u,false,false,0u,1u,0u>)
INSTANCE(BlackOilOnePhaseIndices<0u,0u,0u,0u,false,false,0u,1u,5u>)

// Two phase
INSTANCE(BlackOilTwoPhaseIndices<0u,0u,0u,0u,false,false,0u,0u,0u>)
INSTANCE(BlackOilTwoPhaseIndices<0u,0u,0u,0u,false,false,0u,2u,0u>)
INSTANCE(BlackOilTwoPhaseIndices<0u,0u,1u,0u,false,false,0u,2u,0u>)
INSTANCE(BlackOilTwoPhaseIndices<0u,0u,2u,0u,false,false,0u,2u,0u>)
INSTANCE(BlackOilTwoPhaseIndices<0u,0u,0u,0u,false,true,0u,2u,0u>)
INSTANCE(BlackOilTwoPhaseIndices<0u,0u,0u,0u,false,true,0u,0u,0u>)
INSTANCE(BlackOilTwoPhaseIndices<0u,0u,0u,0u,false,false,0u,1u,0u>)
INSTANCE(BlackOilTwoPhaseIndices<0u,0u,0u,1u,false,false,0u,1u,0u>)

// Blackoil
INSTANCE(BlackOilIndices<0u,0u,0u,0u,false,false,0u,0u>)
INSTANCE(BlackOilIndices<0u,0u,0u,0u,false,false,1u,0u>)
INSTANCE(BlackOilIndices<0u,0u,0u,0u,true,false,0u,0u>)
INSTANCE(BlackOilIndices<0u,0u,0u,0u,false,true,0u,0u>)
INSTANCE(BlackOilIndices<0u,0u,0u,0u,false,true,2u,0u>)
INSTANCE(BlackOilIndices<1u,0u,0u,0u,false,false,0u,0u>)
INSTANCE(BlackOilIndices<0u,1u,0u,0u,false,false,0u,0u>)
INSTANCE(BlackOilIndices<0u,0u,1u,0u,false,false,0u,0u>)
INSTANCE(BlackOilIndices<0u,0u,0u,1u,false,false,0u,0u>)
INSTANCE(BlackOilIndices<0u,0u,0u,1u,false,true,0u,0u>)

}
