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


#ifndef OPM_STANDARDWELL_HEADER_INCLUDED
#define OPM_STANDARDWELL_HEADER_INCLUDED

#include <opm/simulators/timestepping/ConvergenceReport.hpp>
#include <opm/simulators/wells/RateConverter.hpp>
#include <opm/simulators/wells/StandardWellGeneric.hpp>
#include <opm/simulators/wells/VFPInjProperties.hpp>
#include <opm/simulators/wells/VFPProdProperties.hpp>
#include <opm/simulators/wells/WellInterface.hpp>
#include <opm/simulators/wells/WellProdIndexCalculator.hpp>
#include <opm/simulators/wells/ParallelWellInfo.hpp>
#include <opm/simulators/wells/GasLiftSingleWell.hpp>
#include <opm/simulators/wells/GasLiftGroupInfo.hpp>

#include <opm/models/blackoil/blackoilpolymermodules.hh>
#include <opm/models/blackoil/blackoilsolventmodules.hh>
#include <opm/models/blackoil/blackoilextbomodules.hh>
#include <opm/models/blackoil/blackoilfoammodules.hh>
#include <opm/models/blackoil/blackoilbrinemodules.hh>

#include <opm/material/densead/DynamicEvaluation.hpp>
#include <opm/parser/eclipse/EclipseState/Runspec.hpp>
#include <opm/parser/eclipse/EclipseState/Schedule/ScheduleTypes.hpp>

#include <opm/simulators/wells/StandardWellEval.hpp>

#include <dune/common/dynvector.hh>
#include <dune/common/dynmatrix.hh>

#include <memory>
#include <optional>
#include <fmt/format.h>

namespace Opm
{

    template<typename TypeTag>
    class StandardWell : public WellInterface<TypeTag>
                       , public StandardWellEval<GetPropType<TypeTag, Properties::FluidSystem>,
                                                 GetPropType<TypeTag, Properties::Indices>,
                                                 GetPropType<TypeTag, Properties::Scalar>>
    {

    public:
        typedef WellInterface<TypeTag> Base;
        using StdWellEval = StandardWellEval<GetPropType<TypeTag, Properties::FluidSystem>,
                                             GetPropType<TypeTag, Properties::Indices>,
                                             GetPropType<TypeTag, Properties::Scalar>>;

        // TODO: some functions working with AD variables handles only with values (double) without
        // dealing with derivatives. It can be beneficial to make functions can work with either AD or scalar value.
        // And also, it can also be beneficial to make these functions hanle different types of AD variables.
        using typename Base::Simulator;
        using typename Base::IntensiveQuantities;
        using typename Base::FluidSystem;
        using typename Base::MaterialLaw;
        using typename Base::ModelParameters;
        using typename Base::Indices;
        using typename Base::RateConverterType;
        using typename Base::SparseMatrixAdapter;
        using typename Base::FluidState;
        using typename Base::RateVector;
        using typename Base::GasLiftSingleWell;
        using typename Base::GLiftOptWells;
        using typename Base::GLiftProdWells;
        using typename Base::GLiftWellStateMap;
        using typename Base::GLiftSyncGroups;

        using Base::has_solvent;
        using Base::has_zFraction;
        using Base::has_polymer;
        using Base::has_polymermw;
        using Base::has_foam;
        using Base::has_brine;
        using Base::has_energy;

        using PolymerModule =  BlackOilPolymerModule<TypeTag>;
        using FoamModule = BlackOilFoamModule<TypeTag>;
        using BrineModule = BlackOilBrineModule<TypeTag>;

        // number of the conservation equations
        static constexpr int numWellConservationEq = Indices::numPhases + Indices::numSolvents;
        // number of the well control equations
        static constexpr int numWellControlEq = 1;
        // number of the well equations that will always be used
        // based on the solution strategy, there might be other well equations be introduced
        static constexpr int numStaticWellEq = numWellConservationEq + numWellControlEq;

        // the index for Bhp in primary variables and also the index of well control equation
        // they both will be the last one in their respective system.
        // TODO: we should have indices for the well equations and well primary variables separately
        static constexpr int Bhp = numStaticWellEq - numWellControlEq;

        using typename Base::Scalar;


        using Base::name;
        using Base::Water;
        using Base::Oil;
        using Base::Gas;

        using typename Base::BVector;

        using Eval = typename StdWellEval::Eval;
        using EvalWell = typename StdWellEval::EvalWell;
        using BVectorWell = typename StdWellEval::BVectorWell;

        StandardWell(const Well& well,
                     const ParallelWellInfo& pw_info,
                     const int time_step,
                     const ModelParameters& param,
                     const RateConverterType& rate_converter,
                     const int pvtRegionIdx,
                     const int num_components,
                     const int num_phases,
                     const int index_of_well,
                     const std::vector<PerforationData>& perf_data);

        virtual void init(const PhaseUsage* phase_usage_arg,
                          const std::vector<double>& depth_arg,
                          const double gravity_arg,
                          const int num_cells,
                          const std::vector< Scalar >& B_avg) override;


        virtual void initPrimaryVariablesEvaluation() const override;

        /// check whether the well equations get converged for this well
        virtual ConvergenceReport getWellConvergence(const WellState& well_state,
                                                     const std::vector<double>& B_avg,
                                                     DeferredLogger& deferred_logger,
                                                     const bool relax_tolerance = false) const override;

        /// Ax = Ax - C D^-1 B x
        virtual void apply(const BVector& x, BVector& Ax) const override;
        /// r = r - C D^-1 Rw
        virtual void apply(BVector& r) const override;

        /// using the solution x to recover the solution xw for wells and applying
        /// xw to update Well State
        virtual void recoverWellSolutionAndUpdateWellState(const BVector& x,
                                                           WellState& well_state,
                                                           DeferredLogger& deferred_logger) const override;

        /// computing the well potentials for group control
        virtual void computeWellPotentials(const Simulator& ebosSimulator,
                                           const WellState& well_state,
                                           std::vector<double>& well_potentials,
                                           DeferredLogger& deferred_logger) /* const */ override;

        virtual void updatePrimaryVariables(const WellState& well_state, DeferredLogger& deferred_logger) const override;

        virtual void solveEqAndUpdateWellState(WellState& well_state, DeferredLogger& deferred_logger) override;

        virtual void calculateExplicitQuantities(const Simulator& ebosSimulator,
                                                 const WellState& well_state,
                                                 DeferredLogger& deferred_logger) override; // should be const?

        virtual void updateProductivityIndex(const Simulator& ebosSimulator,
                                             const WellProdIndexCalculator& wellPICalc,
                                             WellState& well_state,
                                             DeferredLogger& deferred_logger) const override;

        virtual void  addWellContributions(SparseMatrixAdapter& mat) const override;

        // iterate well equations with the specified control until converged
        bool iterateWellEqWithControl(const Simulator& ebosSimulator,
                                      const double dt,
                                      const Well::InjectionControls& inj_controls,
                                      const Well::ProductionControls& prod_controls,
                                      WellState& well_state,
                                      const GroupState& group_state,
                                      DeferredLogger& deferred_logger) override;

        /// \brief Wether the Jacobian will also have well contributions in it.
        virtual bool jacobianContainsWellContributions() const override
        {
            return param_.matrix_add_well_contributions_;
        }

        virtual void gasLiftOptimizationStage1 (
            WellState& well_state,
            const GroupState& group_state,
            const Simulator& ebosSimulator,
            DeferredLogger& deferred_logger,
            GLiftProdWells &prod_wells,
            GLiftOptWells &glift_wells,
            GLiftWellStateMap &state_map,
            GasLiftGroupInfo &group_info,
            GLiftSyncGroups &sync_groups
        ) const override;

        /* returns BHP */
        double computeWellRatesAndBhpWithThpAlqProd(const Simulator &ebos_simulator,
                               const SummaryState &summary_state,
                               DeferredLogger &deferred_logger,
                               std::vector<double> &potentials,
                               double alq) const;

        void computeWellRatesWithThpAlqProd(
            const Simulator &ebos_simulator,
            const SummaryState &summary_state,
            DeferredLogger &deferred_logger,
            std::vector<double> &potentials,
            double alq) const;

        // NOTE: Cannot be protected since it is used by GasLiftRuntime
        std::optional<double> computeBhpAtThpLimitProdWithAlq(
            const Simulator& ebos_simulator,
            const SummaryState& summary_state,
            DeferredLogger& deferred_logger,
            double alq_value) const;

        // NOTE: Cannot be protected since it is used by GasLiftRuntime
        void computeWellRatesWithBhp(
            const Simulator& ebosSimulator,
            const double& bhp,
            std::vector<double>& well_flux,
            DeferredLogger& deferred_logger) const;

        // NOTE: These cannot be protected since they are used by GasLiftRuntime
        using Base::phaseUsage;
        using Base::vfp_properties_;

        virtual std::vector<double> computeCurrentWellRates(const Simulator& ebosSimulator,
                                                            DeferredLogger& deferred_logger) const override;

        void computeConnLevelProdInd(const FluidState& fs,
                                     const std::function<double(const double)>& connPICalc,
                                     const std::vector<EvalWell>& mobility,
                                     double* connPI) const;

        void computeConnLevelInjInd(const typename StandardWell<TypeTag>::FluidState& fs,
                                    const Phase preferred_phase,
                                    const std::function<double(const double)>& connIICalc,
                                    const std::vector<EvalWell>& mobility,
                                    double* connII,
                                    DeferredLogger& deferred_logger) const;


    protected:
        // protected functions from the Base class
        using Base::getAllowCrossFlow;
        using Base::flowPhaseToEbosCompIdx;
        using Base::flowPhaseToEbosPhaseIdx;
        using Base::ebosCompIdxToFlowCompIdx;
        using Base::wsalt;
        using Base::wsolvent;
        using Base::wpolymer;
        using Base::wfoam;
        using Base::scalingFactor;
        using Base::mostStrictBhpFromBhpLimits;
        using Base::updateWellOperability;
        using Base::checkWellOperability;
        using Base::wellIsStopped;
        using Base::calculateBhpFromThp;
        using Base::getALQ;

        // protected member variables from the Base class
        using Base::current_step_;
        using Base::well_ecl_;
        using Base::gravity_;
        using Base::param_;
        using Base::well_efficiency_factor_;
        using Base::ref_depth_;
        using Base::perf_depth_;
        using Base::well_cells_;
        using Base::number_of_perforations_;
        using Base::number_of_phases_;
        using Base::saturation_table_number_;
        using Base::well_index_;
        using Base::index_of_well_;
        using Base::num_components_;
        using Base::connectionRates_;

        using Base::perf_rep_radius_;
        using Base::perf_length_;
        using Base::bore_diameters_;
        using Base::ipr_a_;
        using Base::ipr_b_;
        using Base::changed_to_stopped_this_step_;

        Eval getPerfCellPressure(const FluidState& fs) const;

        // xw = inv(D)*(rw - C*x)
        void recoverSolutionWell(const BVector& x, BVectorWell& xw) const;

        // updating the well_state based on well solution dwells
        void updateWellState(const BVectorWell& dwells,
                             WellState& well_state,
                             DeferredLogger& deferred_logger) const;

        // calculate the properties for the well connections
        // to calulate the pressure difference between well connections.
        void computePropertiesForWellConnectionPressures(const Simulator& ebosSimulator,
                                                         const WellState& well_state,
                                                         std::vector<double>& b_perf,
                                                         std::vector<double>& rsmax_perf,
                                                         std::vector<double>& rvmax_perf,
                                                         std::vector<double>& surf_dens_perf) const;

        void computeWellConnectionDensitesPressures(const Simulator& ebosSimulator,
                                                    const WellState& well_state,
                                                    const std::vector<double>& b_perf,
                                                    const std::vector<double>& rsmax_perf,
                                                    const std::vector<double>& rvmax_perf,
                                                    const std::vector<double>& surf_dens_perf);

        void computeWellConnectionPressures(const Simulator& ebosSimulator,
                                            const WellState& well_state);

        void computePerfRateEval(const IntensiveQuantities& intQuants,
                                 const std::vector<EvalWell>& mob,
                                 const EvalWell& bhp,
                                 const double Tw,
                                 const int perf,
                                 const bool allow_cf,
                                 std::vector<EvalWell>& cq_s,
                                 double& perf_dis_gas_rate,
                                 double& perf_vap_oil_rate,
                                 DeferredLogger& deferred_logger) const;

        void computePerfRateScalar(const IntensiveQuantities& intQuants,
                                   const std::vector<Scalar>& mob,
                                   const Scalar& bhp,
                                   const double Tw,
                                   const int perf,
                                   const bool allow_cf,
                                   std::vector<Scalar>& cq_s,
                                   DeferredLogger& deferred_logger) const;

        template<class Value>
        void computePerfRate(const std::vector<Value>& mob,
                             const Value& pressure,
                             const Value& bhp,
                             const Value& rs,
                             const Value& rv,
                             std::vector<Value>& b_perfcells_dense,
                             const double Tw,
                             const int perf,
                             const bool allow_cf,
                             const Value& skin_pressure,
                             const std::vector<Value>& cmix_s,
                             std::vector<Value>& cq_s,
                             double& perf_dis_gas_rate,
                             double& perf_vap_oil_rate,
                             DeferredLogger& deferred_logger) const;

        void computeWellRatesWithBhpPotential(const Simulator& ebosSimulator,
                                              const double& bhp,
                                              std::vector<double>& well_flux,
                                              DeferredLogger& deferred_logger) const;

        std::vector<double> computeWellPotentialWithTHP(
            const Simulator& ebosSimulator,
            DeferredLogger& deferred_logger,
            const WellState &well_state) const;


        virtual double getRefDensity() const override;

        // get the mobility for specific perforation
        void getMobilityEval(const Simulator& ebosSimulator,
                             const int perf,
                             std::vector<EvalWell>& mob,
                             DeferredLogger& deferred_logger) const;

        // get the mobility for specific perforation
        void getMobilityScalar(const Simulator& ebosSimulator,
                               const int perf,
                               std::vector<Scalar>& mob,
                               DeferredLogger& deferred_logger) const;


        void updateWaterMobilityWithPolymer(const Simulator& ebos_simulator,
                                            const int perf,
                                            std::vector<EvalWell>& mob_water,
                                            DeferredLogger& deferred_logger) const;

        void updatePrimaryVariablesNewton(const BVectorWell& dwells,
                                          const WellState& well_state) const;

        // update extra primary vriables if there are any
        void updateExtraPrimaryVariables(const BVectorWell& dwells) const;


        void updateWellStateFromPrimaryVariables(WellState& well_state, DeferredLogger& deferred_logger) const;

        virtual void assembleWellEqWithoutIteration(const Simulator& ebosSimulator,
                                                    const double dt,
                                                    const Well::InjectionControls& inj_controls,
                                                    const Well::ProductionControls& prod_controls,
                                                    WellState& well_state,
                                                    const GroupState& group_state,
                                                    DeferredLogger& deferred_logger) override;

        void assembleWellEqWithoutIterationImpl(const Simulator& ebosSimulator,
                                                const double dt,
                                                WellState& well_state,
                                                const GroupState& group_state,
                                                DeferredLogger& deferred_logger);

        void calculateSinglePerf(const Simulator& ebosSimulator,
                                 const int perf,
                                 WellState& well_state,
                                 std::vector<RateVector>& connectionRates,
                                 std::vector<EvalWell>& cq_s,
                                 EvalWell& water_flux_s,
                                 EvalWell& cq_s_zfrac_effective,
                                 DeferredLogger& deferred_logger) const;

        // check whether the well is operable under BHP limit with current reservoir condition
        virtual void checkOperabilityUnderBHPLimitProducer(const WellState& well_state, const Simulator& ebos_simulator, DeferredLogger& deferred_logger) override;

        // check whether the well is operable under THP limit with current reservoir condition
        virtual void checkOperabilityUnderTHPLimitProducer(const Simulator& ebos_simulator, const WellState& well_state, DeferredLogger& deferred_logger) override;

        // updating the inflow based on the current reservoir condition
        virtual void updateIPR(const Simulator& ebos_simulator, DeferredLogger& deferred_logger) const override;

        // for a well, when all drawdown are in the wrong direction, then this well will not
        // be able to produce/inject .
        bool allDrawDownWrongDirection(const Simulator& ebos_simulator) const;

        // whether the well can produce / inject based on the current well state (bhp)
        bool canProduceInjectWithCurrentBhp(const Simulator& ebos_simulator,
                                            const WellState& well_state,
                                            DeferredLogger& deferred_logger);

        // turn on crossflow to avoid singular well equations
        // when the well is banned from cross-flow and the BHP is not properly initialized,
        // we turn on crossflow to avoid singular well equations. It can result in wrong-signed
        // well rates, it can cause problem for THP calculation
        // TODO: looking for better alternative to avoid wrong-signed well rates
        bool openCrossFlowAvoidSingularity(const Simulator& ebos_simulator) const;

        // calculate the skin pressure based on water velocity, throughput and polymer concentration.
        // throughput is used to describe the formation damage during water/polymer injection.
        // calculated skin pressure will be applied to the drawdown during perforation rate calculation
        // to handle the effect from formation damage.
        EvalWell pskin(const double throuhgput,
                       const EvalWell& water_velocity,
                       const EvalWell& poly_inj_conc,
                       DeferredLogger& deferred_logger) const;

        // calculate the skin pressure based on water velocity, throughput during water injection.
        EvalWell pskinwater(const double throughput,
                            const EvalWell& water_velocity,
                            DeferredLogger& deferred_logger) const;

        // calculate the injecting polymer molecular weight based on the througput and water velocity
        EvalWell wpolymermw(const double throughput,
                            const EvalWell& water_velocity,
                            DeferredLogger& deferred_logger) const;

        // modify the water rate for polymer injectivity study
        void handleInjectivityRate(const Simulator& ebosSimulator,
                                   const int perf,
                                   std::vector<EvalWell>& cq_s) const;

        // handle the extra equations for polymer injectivity study
        void handleInjectivityEquations(const Simulator& ebosSimulator,
                                        const WellState& well_state,
                                        const int perf,
                                        const EvalWell& water_flux_s,
                                        DeferredLogger& deferred_logger);

        virtual void updateWaterThroughput(const double dt, WellState& well_state) const override;

        // checking convergence of extra equations, if there are any
        void checkConvergenceExtraEqs(const std::vector<double>& res,
                                      ConvergenceReport& report) const;

        // updating the connectionRates_ related polymer molecular weight
        void updateConnectionRatePolyMW(const EvalWell& cq_s_poly,
                                        const IntensiveQuantities& int_quants,
                                        const WellState& well_state,
                                        const int perf,
                                        std::vector<RateVector>& connectionRates,
                                        DeferredLogger& deferred_logger) const;


        std::optional<double> computeBhpAtThpLimitProd(const WellState& well_state,
                                                       const Simulator& ebos_simulator,
                                                       const SummaryState& summary_state,
                                                       DeferredLogger& deferred_logger) const;

        std::optional<double> computeBhpAtThpLimitInj(const Simulator& ebos_simulator,
                                                      const SummaryState& summary_state,
                                                      DeferredLogger& deferred_logger) const;

    };

}

#include "StandardWell_impl.hpp"

#endif // OPM_STANDARDWELL_HEADER_INCLUDED
