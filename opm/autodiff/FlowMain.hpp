/*
  Copyright 2013, 2014, 2015 SINTEF ICT, Applied Mathematics.
  Copyright 2014 Dr. Blatt - HPC-Simulation-Software & Services
  Copyright 2015 IRIS AS
  Copyright 2014 STATOIL ASA.

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

#ifndef OPM_FLOWMAIN_HEADER_INCLUDED
#define OPM_FLOWMAIN_HEADER_INCLUDED


#include <opm/common/utility/platform_dependent/disable_warnings.h>

#include <dune/common/version.hh>
#if DUNE_VERSION_NEWER(DUNE_COMMON, 2, 3)
#include <dune/common/parallel/mpihelper.hh>
#else
#include <dune/common/mpihelper.hh>
#endif

#include <opm/common/utility/platform_dependent/reenable_warnings.h>


#include <opm/core/grid/GridManager.hpp>
#include <opm/autodiff/GridHelpers.hpp>
#include <opm/autodiff/createGlobalCellArray.hpp>
#include <opm/autodiff/GridInit.hpp>

#include <opm/core/wells.h>
#include <opm/core/wells/WellsManager.hpp>
#include <opm/common/ErrorMacros.hpp>
#include <opm/core/simulator/initState.hpp>
#include <opm/core/simulator/initStateEquil.hpp>
#include <opm/core/simulator/SimulatorReport.hpp>
#include <opm/core/simulator/SimulatorTimer.hpp>
#include <opm/core/utility/miscUtilities.hpp>
#include <opm/core/utility/parameters/ParameterGroup.hpp>
#include <opm/simulators/thresholdPressures.hpp> // Note: the GridHelpers must be included before this (to make overloads available). \TODO: Fix.

#include <opm/material/fluidmatrixinteractions/EclMaterialLawManager.hpp>
#include <opm/core/props/BlackoilPropertiesBasic.hpp>
#include <opm/core/props/BlackoilPropertiesFromDeck.hpp>
#include <opm/core/props/rock/RockCompressibility.hpp>
#include <opm/core/props/satfunc/RelpermDiagnostics.hpp>
#include <opm/core/linalg/LinearSolverFactory.hpp>
#include <opm/autodiff/NewtonIterationBlackoilSimple.hpp>
#include <opm/autodiff/NewtonIterationBlackoilCPR.hpp>
#include <opm/autodiff/NewtonIterationBlackoilInterleaved.hpp>

#include <opm/autodiff/WellStateFullyImplicitBlackoil.hpp>

#include <opm/autodiff/BlackoilPropsAdFromDeck.hpp>
#include <opm/autodiff/RedistributeDataHandles.hpp>
#include <opm/autodiff/moduleVersion.hpp>
#include <opm/autodiff/MissingFeatures.hpp>

#include <opm/core/utility/share_obj.hpp>
#include <opm/core/utility/initHydroCarbonState.hpp>
#include <opm/common/OpmLog/OpmLog.hpp>
#include <opm/common/OpmLog/EclipsePRTLog.hpp>
#include <opm/common/OpmLog/LogUtil.hpp>
#include <opm/parser/eclipse/Deck/Deck.hpp>
#include <opm/parser/eclipse/Parser/Parser.hpp>
#include <opm/parser/eclipse/Parser/ParseContext.hpp>
#include <opm/parser/eclipse/EclipseState/EclipseState.hpp>
#include <opm/parser/eclipse/EclipseState/IOConfig/IOConfig.hpp>
#include <opm/parser/eclipse/EclipseState/InitConfig/InitConfig.hpp>
#include <opm/parser/eclipse/EclipseState/checkDeck.hpp>

#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>

#ifdef _OPENMP
#include <omp.h>
#endif

#include <memory>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <numeric>
#include <cstdlib>
#include <stdexcept>




namespace Opm
{


    namespace detail
    {
        boost::filesystem::path simulationCaseName( const std::string& casename );
        int64_t convertMessageType(const Message::type& mtype);
    }






    /// This class encapsulates the setup and running of
    /// a simulator based on an input deck.
    template <class Implementation, class Grid, class Simulator>
    class FlowMainBase
    {
    public:


        /// This is the main function of Flow.
        /// It runs a complete simulation, with the given grid and
        /// simulator classes, based on user command-line input.  The
        /// content of this function used to be in the main() function of
        /// flow.cpp.
        int execute(int argc, char** argv)
        try {
            // Setup.
            asImpl().setupParallelism(argc, argv);
            asImpl().printStartupMessage();
            const bool ok = asImpl().setupParameters(argc, argv);
            if (!ok) {
                return EXIT_FAILURE;
            }
            asImpl().setupOutput();
            asImpl().setupLogging();
            asImpl().readDeckInput();
            asImpl().setupGridAndProps();
            asImpl().extractMessages();
            asImpl().runDiagnostics();
            asImpl().setupState();
            asImpl().writeInit();
            asImpl().distributeData();
            asImpl().setupOutputWriter();
            asImpl().setupLinearSolver();
            asImpl().createSimulator();
            
            // Run.
            return asImpl().runSimulator();
        }
        catch (const std::exception &e) {
            std::ostringstream message;
            message  << "Program threw an exception: " << e.what();

            if( output_cout_ )
            {
                OpmLog::error(message.str());
            }

            return EXIT_FAILURE;
        }



    protected:





        // ------------   Types   ------------


        typedef BlackoilPropsAdFromDeck FluidProps;
        typedef FluidProps::MaterialLawManager MaterialLawManager;
        typedef typename Simulator::ReservoirState ReservoirState;


        // ------------   Data members   ------------


        // The comments indicate in which method the
        // members first occur.

        // setupParallelism()
        int  mpi_rank_ = 0;
        bool output_cout_ = false;
        bool must_distribute_ = false;
        // setupParameters()
        parameter::ParameterGroup param_;
        // setupOutput()
        bool output_to_files_ = false;
        std::string output_dir_ = std::string(".");
        // readDeckInput()
        std::shared_ptr<const Deck> deck_;
        std::shared_ptr<EclipseState> eclipse_state_;
        // setupGridAndProps()
        std::unique_ptr<GridInit<Grid>> grid_init_;
        std::shared_ptr<MaterialLawManager> material_law_manager_;
        std::unique_ptr<FluidProps> fluidprops_;
        std::unique_ptr<RockCompressibility> rock_comp_;
        std::array<double, 3> gravity_;
        bool use_local_perm_ = true;
        std::unique_ptr<DerivedGeology> geoprops_;
        // setupState()
        std::unique_ptr<ReservoirState> state_;

        std::vector<double> threshold_pressures_;
        // distributeData()
        boost::any parallel_information_;
        // setupOutputWriter()
        std::unique_ptr<BlackoilOutputWriter> output_writer_;
        // setupLinearSolver
        std::unique_ptr<NewtonIterationBlackoilInterface> fis_solver_;
        // createSimulator()
        std::unique_ptr<Simulator> simulator_;
        // create log file
        std::string logFile_;
        // The names of wells that are artifically defunct in parallel runs.
        // Those wells are handled on a another process.
        std::unordered_set<std::string> defunct_well_names_;
        // ------------   Methods   ------------


        // Set up MPI and OpenMP.
        // Writes to:
        //   output_cout_
        //   must_distribute_
        void setupParallelism(int argc, char** argv)
        {
            // MPI setup.
            // Must ensure an instance of the helper is created to initialise MPI.
            // For a build without MPI the Dune::FakeMPIHelper is used, so rank will
            // be 0 and size 1.
            const Dune::MPIHelper& mpi_helper = Dune::MPIHelper::instance(argc, argv);
            mpi_rank_ = mpi_helper.rank();
            const int mpi_size = mpi_helper.size();
            output_cout_ = ( mpi_rank_ == 0 );
            must_distribute_ = ( mpi_size > 1 );

#ifdef _OPENMP
            // OpenMP setup.
            if (!getenv("OMP_NUM_THREADS")) {
                // Default to at most 4 threads, regardless of
                // number of cores (unless ENV(OMP_NUM_THREADS) is defined)
                int num_cores = omp_get_num_procs();
                int num_threads = std::min(4, num_cores);
                omp_set_num_threads(num_threads);
            }
#pragma omp parallel
            if (omp_get_thread_num() == 0) {
                // omp_get_num_threads() only works as expected within a parallel region.
                const int num_omp_threads = omp_get_num_threads();
                if (mpi_size == 1) {
                    std::cout << "OpenMP using " << num_omp_threads << " threads." << std::endl;
                } else {
                    std::cout << "OpenMP using " << num_omp_threads << " threads on MPI rank " << mpi_rank_ << "." << std::endl;
                }
            }
#endif
        }

        /// checks cartesian adjacency of global indices g1 and g2
        bool cartesianAdjacent(const Grid& grid, int g1, int g2) {
            // we need cartDims from UgGridHelpers
            using namespace UgGridHelpers;

            int diff = std::abs(g1 - g2);

            const int * dimens = cartDims(grid);
            if (diff == 1)
               return true;
            if (diff == dimens[0])
               return true;
            if (diff == dimens[0] * dimens[1])
               return true;

            return false;
        }

        // Print startup message if on output rank.
        void printStartupMessage()
        {
            if (output_cout_) {
                const std::string version = moduleVersionName();
                std::cout << "**********************************************************************\n";
                std::cout << "*                                                                    *\n";
                std::cout << "*                   This is Flow (version " << version << ")"
                          << std::string(26 - version.size(), ' ') << "*\n";
                std::cout << "*                                                                    *\n";
                std::cout << "* Flow is a simulator for fully implicit three-phase black-oil flow, *\n";
                std::cout << "*            and is part of OPM. For more information see:           *\n";
                std::cout << "*                       http://opm-project.org                       *\n";
                std::cout << "*                                                                    *\n";
                std::cout << "**********************************************************************\n\n";
            }
        }

        // Read parameters, see if a deck was specified on the command line, and if
        // it was, insert it into parameters.
        // Writes to:
        //   param_
        // Returns true if ok, false if not.
        bool setupParameters(int argc, char** argv)
        {
            param_ = parameter::ParameterGroup(argc, argv, false, output_cout_);

            // See if a deck was specified on the command line.
            if (!param_.unhandledArguments().empty()) {
                if (param_.unhandledArguments().size() != 1) {
                    std::cerr << "You can only specify a single input deck on the command line.\n";
                    return false;
                } else {
                    const auto casename = detail::simulationCaseName( param_.unhandledArguments()[ 0 ] );
                    param_.insertParameter("deck_filename", casename.string() );
                }
            }

            // We must have an input deck. Grid and props will be read from that.
            if (!param_.has("deck_filename")) {
                std::cerr << "This program must be run with an input deck.\n"
                    "Specify the deck filename either\n"
                    "    a) as a command line argument by itself\n"
                    "    b) as a command line parameter with the syntax deck_filename=<path to your deck>, or\n"
                    "    c) as a parameter in a parameter file (.param or .xml) passed to the program.\n";
                return false;
            }
            return true;
        }





        // Set output_to_files_ and set/create output dir. Write parameter file.
        // Writes to:
        //   output_to_files_
        //   output_dir_
        // Throws std::runtime_error if failed to create (if requested) output dir.
        void setupOutput()
        {
            // Write parameters used for later reference. (only if rank is zero)
            output_to_files_ = output_cout_ && param_.getDefault("output", true);
            // Always read output_dir as it will be set unconditionally later.
            // Not doing this might cause files to be created in the current
            // directory.
            output_dir_ =
                param_.getDefault("output_dir", std::string("."));

            if (output_to_files_) {
                // Create output directory if needed.
                boost::filesystem::path fpath(output_dir_);
                if (!is_directory(fpath)) {
                    try {
                        create_directories(fpath);
                    }
                    catch (...) {
                        OPM_THROW(std::runtime_error, "Creating directories failed: " << fpath);
                    }
                }
                // Write simulation parameters.
                param_.writeParam(output_dir_ + "/simulation.param");
            }
        }





        // Setup OpmLog backend with output_dir. 
        void setupLogging()
        {
            std::string deck_filename = param_.get<std::string>("deck_filename");
            // create logFile
            using boost::filesystem::path; 
            path fpath(deck_filename);
            std::string baseName;
            std::ostringstream debugFileStream;
            std::ostringstream logFileStream;

            if (boost::to_upper_copy(path(fpath.extension()).string()) == ".DATA") {
                baseName = path(fpath.stem()).string();
            } else {
                baseName = path(fpath.filename()).string();
            }
            if (param_.has("output_dir")) {
                logFileStream << output_dir_ << "/" << baseName + ".PRT";
                debugFileStream << output_dir_ + "/." + baseName + ".DEBUG";
            } else {
                logFileStream << baseName << ".PRT";
                debugFileStream << "." << baseName << ".DEBUG";
            }
            {
                // Added rank to log file for non-zero ranks.
                // This prevents message loss.
                debugFileStream << "."<< mpi_rank_;
                // If the following file appears then there is a bug.
                logFileStream << "." << mpi_rank_;
            }
            std::string debugFile = debugFileStream.str();
            logFile_ = logFileStream.str();

            std::shared_ptr<EclipsePRTLog> prtLog = std::make_shared<EclipsePRTLog>(logFile_ , Log::NoDebugMessageTypes, false, output_cout_);
            std::shared_ptr<StreamLog> streamLog = std::make_shared<StreamLog>(std::cout, Log::StdoutMessageTypes);
            OpmLog::addBackend( "ECLIPSEPRTLOG" , prtLog );
            OpmLog::addBackend( "STREAMLOG", streamLog);
            std::shared_ptr<StreamLog> debugLog = std::make_shared<EclipsePRTLog>(debugFile, Log::DefaultMessageTypes, false, output_cout_);
            OpmLog::addBackend( "DEBUGLOG" ,  debugLog);
            prtLog->setMessageFormatter(std::make_shared<SimpleMessageFormatter>(false));
            streamLog->setMessageLimiter(std::make_shared<MessageLimiter>(10));
            streamLog->setMessageFormatter(std::make_shared<SimpleMessageFormatter>(true));
            // Read parameters.
            if ( output_cout_ )
            {
                OpmLog::debug("\n---------------    Reading parameters     ---------------\n");
            }
        }






        // Parser the input and creates the Deck and EclipseState objects.
        // Writes to:
        //   deck_
        //   eclipse_state_
        // May throw if errors are encountered, here configured to be somewhat tolerant.
        void readDeckInput()
        {
            std::string deck_filename = param_.get<std::string>("deck_filename");

            // Create Parser
            ParserPtr parser(new Parser());

            // Create Deck and EclipseState.
            try {
                ParseContext parseContext({{ ParseContext::PARSE_RANDOM_SLASH , InputError::IGNORE }});
                deck_ = parser->parseFile(deck_filename, parseContext);
                checkDeck(deck_, parser);

                if ( output_cout_)
                {
                    MissingFeatures::checkKeywords(*deck_);
                }

                eclipse_state_.reset(new EclipseState(*deck_, parseContext));
                auto ioConfig = eclipse_state_->getIOConfig();
                ioConfig->setOutputDir(output_dir_);
            }
            catch (const std::invalid_argument& e) {
                std::cerr << "Failed to create valid EclipseState object. See logfile: " << logFile_ << std::endl;
                std::cerr << "Exception caught: " << e.what() << std::endl;
                throw;
            }

            // Possibly override IOConfig setting (from deck) for how often RESTART files should get written to disk (every N report step)
            if (param_.has("output_interval")) {
                const int output_interval = param_.get<int>("output_interval");
                eclipse_state_->getRestartConfig().overrideRestartWriteInterval( size_t( output_interval ) );
            }

            // Possible to force initialization only behavior (NOSIM).
            if (param_.has("nosim")) {
                const bool nosim = param_.get<bool>("nosim");
                IOConfigPtr ioConfig = eclipse_state_->getIOConfig();
                ioConfig->overrideNOSIM( nosim );
            }
        }





        // Create grid and property objects.
        // Writes to:
        //   grid_init_
        //   material_law_manager_
        //   fluidprops_
        //   rock_comp_
        //   gravity_
        //   use_local_perm_
        //   geoprops_
        void setupGridAndProps()
        {
            // Create grid.
            const std::vector<double>& porv =
                    eclipse_state_->get3DProperties().getDoubleGridProperty("PORV").getData();
            grid_init_.reset(new GridInit<Grid>(eclipse_state_, porv));
            const Grid& grid = grid_init_->grid();

            // Create material law manager.
            std::vector<int> compressedToCartesianIdx;
            Opm::createGlobalCellArray(grid, compressedToCartesianIdx);
            material_law_manager_.reset(new MaterialLawManager());
            material_law_manager_->initFromDeck(deck_, eclipse_state_, compressedToCartesianIdx);

            // Rock and fluid properties.
            fluidprops_.reset(new BlackoilPropsAdFromDeck(deck_, eclipse_state_, material_law_manager_, grid));

            // Rock compressibility.
            rock_comp_.reset(new RockCompressibility(deck_, eclipse_state_));

            // Gravity.
            assert(UgGridHelpers::dimensions(grid) == 3);
            gravity_.fill(0.0);
            gravity_[2] = deck_->hasKeyword("NOGRAV")
                ? param_.getDefault("gravity", 0.0)
                : param_.getDefault("gravity", unit::gravity);

            // Geological properties
            use_local_perm_ = param_.getDefault("use_local_perm", use_local_perm_);
            geoprops_.reset(new DerivedGeology(grid, *fluidprops_, eclipse_state_, use_local_perm_, gravity_.data()));
        }





        // Initialise the reservoir state. Updated fluid props for SWATINIT.
        // Writes to:
        //   state_
        //   threshold_pressures_
        //   fluidprops_ (if SWATINIT is used)
        void setupState()
        {
            const PhaseUsage pu = Opm::phaseUsageFromDeck(deck_);
            const Grid& grid = grid_init_->grid();

            // Need old-style fluid object for init purposes (only).
            BlackoilPropertiesFromDeck props( deck_, eclipse_state_, material_law_manager_,
                                              Opm::UgGridHelpers::numCells(grid),
                                              Opm::UgGridHelpers::globalCell(grid),
                                              Opm::UgGridHelpers::cartDims(grid),
                                              param_);


            // Init state variables (saturation and pressure).
            if (param_.has("init_saturation")) {
                state_.reset( new ReservoirState( Opm::UgGridHelpers::numCells(grid),
                                                  Opm::UgGridHelpers::numFaces(grid),
                                                  props.numPhases() ));

                initStateBasic(Opm::UgGridHelpers::numCells(grid),
                               Opm::UgGridHelpers::globalCell(grid),
                               Opm::UgGridHelpers::cartDims(grid),
                               Opm::UgGridHelpers::numFaces(grid),
                               Opm::UgGridHelpers::faceCells(grid),
                               Opm::UgGridHelpers::beginFaceCentroids(grid),
                               Opm::UgGridHelpers::beginCellCentroids(grid),
                               Opm::UgGridHelpers::dimensions(grid),
                               props, param_, gravity_[2], *state_);

                initBlackoilSurfvol(Opm::UgGridHelpers::numCells(grid), props, *state_);

                enum { Oil = BlackoilPhases::Liquid, Gas = BlackoilPhases::Vapour };
                if (pu.phase_used[Oil] && pu.phase_used[Gas]) {
                    const int numPhases = props.numPhases();
                    const int numCells  = Opm::UgGridHelpers::numCells(grid);

                    // Uglyness 1: The state is a templated type, here we however make explicit use BlackoilState.
                    auto& gor = state_->getCellData( BlackoilState::GASOILRATIO );
                    const auto& surface_vol = state_->getCellData( BlackoilState::SURFACEVOL );
                    for (int c = 0; c < numCells; ++c) {
                        // Uglyness 2: Here we explicitly use the layout of the saturation in the surface_vol field.
                        gor[c] = surface_vol[ c * numPhases + pu.phase_pos[Gas]] / surface_vol[ c * numPhases + pu.phase_pos[Oil]];
                    }
                }
            } else if (deck_->hasKeyword("EQUIL")) {
                // Which state class are we really using - what a f... mess?
                state_.reset( new ReservoirState( Opm::UgGridHelpers::numCells(grid),
                                                  Opm::UgGridHelpers::numFaces(grid),
                                                  props.numPhases()));

                initStateEquil(grid, props, deck_, eclipse_state_, gravity_[2], *state_);
                //state_.faceflux().resize(Opm::UgGridHelpers::numFaces(grid), 0.0);
            } else {
                state_.reset( new ReservoirState( Opm::UgGridHelpers::numCells(grid),
                                                  Opm::UgGridHelpers::numFaces(grid),
                                                  props.numPhases()));
                initBlackoilStateFromDeck(Opm::UgGridHelpers::numCells(grid),
                                          Opm::UgGridHelpers::globalCell(grid),
                                          Opm::UgGridHelpers::numFaces(grid),
                                          Opm::UgGridHelpers::faceCells(grid),
                                          Opm::UgGridHelpers::beginFaceCentroids(grid),
                                          Opm::UgGridHelpers::beginCellCentroids(grid),
                                          Opm::UgGridHelpers::dimensions(grid),
                                          props, deck_, gravity_[2], *state_);
            }

            // Threshold pressures.
            std::map<std::pair<int, int>, double> maxDp;
            computeMaxDp(maxDp, deck_, eclipse_state_, grid_init_->grid(), *state_, props, gravity_[2]);
            threshold_pressures_ = thresholdPressures(deck_, eclipse_state_, grid, maxDp);
            std::vector<double> threshold_pressures_nnc = thresholdPressuresNNC(eclipse_state_, geoprops_->nnc(), maxDp);
            threshold_pressures_.insert(threshold_pressures_.end(), threshold_pressures_nnc.begin(), threshold_pressures_nnc.end());

            // The capillary pressure is scaled in fluidprops_ to match the scaled capillary pressure in props.
            if (deck_->hasKeyword("SWATINIT")) {
                const int numCells = Opm::UgGridHelpers::numCells(grid);
                std::vector<int> cells(numCells);
                for (int c = 0; c < numCells; ++c) { cells[c] = c; }
                std::vector<double> pc = state_->saturation();
                props.capPress(numCells, state_->saturation().data(), cells.data(), pc.data(), nullptr);
                fluidprops_->setSwatInitScaling(state_->saturation(), pc);
            }
            initHydroCarbonState(*state_, pu, Opm::UgGridHelpers::numCells(grid), deck_->hasKeyword("DISGAS"), deck_->hasKeyword("VAPOIL"));


        }





        // Distribute the grid, properties and state.
        // Writes to:
        //   grid_init_->grid()
        //   state_
        //   fluidprops_
        //   geoprops_
        //   material_law_manager_
        //   parallel_information_
        void distributeData()
        {
            // At this point all properties and state variables are correctly initialized
            // If there are more than one processors involved, we now repartition the grid
            // and initilialize new properties and states for it.
            if (must_distribute_) {
                defunct_well_names_ =
                    distributeGridAndData(grid_init_->grid(), deck_, eclipse_state_,
                                          *state_, *fluidprops_, *geoprops_,
                                          material_law_manager_, threshold_pressures_,
                                          parallel_information_, use_local_perm_);
            }
        }





        // Extract messages from parser.
        // Writes to:
        //    OpmLog singleton.
        void extractMessages()
        {
            if ( !output_cout_ )
                return;

            auto extractMessage = [](const Message& msg) {
                auto log_type = detail::convertMessageType(msg.mtype);
                const auto& location = msg.location;
                if (location) {
                    OpmLog::addMessage(log_type, Log::fileMessage(location.filename, location.lineno, msg.message));
                } else {
                    OpmLog::addMessage(log_type, msg.message);
                }
            };

            // Extract messages from Deck.
            for(const auto& msg : deck_->getMessageContainer()) {
                extractMessage(msg);
            }

            // Extract messages from EclipseState.
            for (const auto& msg : eclipse_state_->getMessageContainer()) {
                extractMessage(msg);
            }
        }





        // Run diagnostics.
        // Writes to:
        //   OpmLog singleton.
        void runDiagnostics()
        {
            if( ! output_cout_ )
            {
                return;
            }

            // Run relperm diagnostics
            RelpermDiagnostics diagnostic;
            diagnostic.diagnosis(eclipse_state_, deck_, grid_init_->grid());
        }


        void writeInit()
        {
            bool output      = param_.getDefault("output", true);
            bool output_ecl  = param_.getDefault("output_ecl", true);
            const Grid& grid = grid_init_->grid();
            if( output && output_ecl && output_cout_)
            {
                const EclipseGrid& inputGrid = *eclipse_state_->getInputGrid();
                EclipseWriter writer(eclipse_state_, UgGridHelpers::createEclipseGrid( grid , inputGrid ));
                writer.writeInitAndEgrid(geoprops_->simProps(grid),
                                         geoprops_->nonCartesianConnections());
            }
        }


        // Setup output writer.
        // Writes to:
        //   output_writer_
        void setupOutputWriter()
        {
            // create output writer after grid is distributed, otherwise the parallel output
            // won't work correctly since we need to create a mapping from the distributed to
            // the global view
            output_writer_.reset(new BlackoilOutputWriter(grid_init_->grid(),
                                                          param_,
                                                          eclipse_state_,
                                                          Opm::phaseUsageFromDeck(deck_),
                                                          fluidprops_->permeability()));
        }





        // Setup linear solver.
        // Writes to:
        //   fis_solver_
        void setupLinearSolver()
        {
            const std::string cprSolver = "cpr";
            const std::string interleavedSolver = "interleaved";
            const std::string directSolver = "direct";
            std::string flowDefaultSolver = interleavedSolver;

            if (!param_.has("solver_approach")) {
                if (eclipse_state_->getSimulationConfig().useCPR()) {
                    flowDefaultSolver = cprSolver;
                }
            }

            const std::string solver_approach = param_.getDefault("solver_approach", flowDefaultSolver);

            if (solver_approach == cprSolver) {
                fis_solver_.reset(new NewtonIterationBlackoilCPR(param_, parallel_information_));
            } else if (solver_approach == interleavedSolver) {
                fis_solver_.reset(new NewtonIterationBlackoilInterleaved(param_, parallel_information_));
            } else if (solver_approach == directSolver) {
                fis_solver_.reset(new NewtonIterationBlackoilSimple(param_, parallel_information_));
            } else {
                OPM_THROW( std::runtime_error , "Internal error - solver approach " << solver_approach << " not recognized.");
            }
        }





        // Run the simulator.
        // Returns EXIT_SUCCESS if it does not throw.
        int runSimulator()
        {
            Opm::ScheduleConstPtr schedule = eclipse_state_->getSchedule();
            Opm::TimeMapConstPtr timeMap(schedule->getTimeMap());
            std::shared_ptr<IOConfig> ioConfig = eclipse_state_->getIOConfig();
            SimulatorTimer simtimer;

            // initialize variables
            const auto& initConfig = eclipse_state_->getInitConfig();
            simtimer.init(timeMap, (size_t)initConfig.getRestartStep());

            if (!ioConfig->initOnly()) {
                if (output_cout_) {
                    std::string msg;
                    msg = "\n\n================ Starting main simulation loop ===============\n";
                    OpmLog::info(msg);
                }

                SimulatorReport fullReport = simulator_->run(simtimer, *state_);

                if (output_cout_) {
                    std::ostringstream ss;
                    ss << "\n\n================    End of simulation     ===============\n\n";
                    fullReport.reportFullyImplicit(ss);
                    OpmLog::info(ss.str());
                    if (param_.anyUnused()) {
                        // This allows a user to catch typos and misunderstandings in the
                        // use of simulator parameters.
                        std::cout << "--------------------   Unused parameters:   --------------------\n";
                        param_.displayUsage();
                        std::cout << "----------------------------------------------------------------" << std::endl;
                    }
                }

                if (output_to_files_) {
                    std::string filename = output_dir_ + "/walltime.txt";
                    std::fstream tot_os(filename.c_str(), std::fstream::trunc | std::fstream::out);
                    fullReport.reportParam(tot_os);
                }
            } else {
                if (output_cout_) {
                    std::cout << "\n\n================ Simulation turned off ===============\n" << std::flush;
                }

            }
            return EXIT_SUCCESS;
        }





        // Access the most-derived class used for
        // static polymorphism (CRTP).
        Implementation& asImpl()
        {
            return static_cast<Implementation&>(*this);
        }


    }; // class FlowMainBase






    // The FlowMain class is the basic black-oil simulator case.
    template <class Grid, class Simulator>
    class FlowMain : public FlowMainBase<FlowMain<Grid, Simulator>, Grid, Simulator>
    {
    protected:
        using Base = FlowMainBase<FlowMain<Grid, Simulator>, Grid, Simulator>;
        friend Base;

        // Create simulator instance.
        // Writes to:
        //   simulator_
        void createSimulator()
        {
            // Create the simulator instance.
            Base::simulator_.reset(new Simulator(Base::param_,
                                                 Base::grid_init_->grid(),
                                                 *Base::geoprops_,
                                                 *Base::fluidprops_,
                                                 Base::rock_comp_->isActive() ? Base::rock_comp_.get() : nullptr,
                                                 *Base::fis_solver_,
                                                 Base::gravity_.data(),
                                                 Base::deck_->hasKeyword("DISGAS"),
                                                 Base::deck_->hasKeyword("VAPOIL"),
                                                 Base::eclipse_state_,
                                                 *Base::output_writer_,
                                                 Base::threshold_pressures_,
                                                 Base::defunct_well_names_));
        }
    };






    namespace detail
    {

        boost::filesystem::path simulationCaseName( const std::string& casename ) {
            namespace fs = boost::filesystem;

            const auto exists = []( const fs::path& f ) -> bool {
                if( !fs::exists( f ) ) return false;

                if( fs::is_regular_file( f ) ) return true;

                return fs::is_symlink( f )
                && fs::is_regular_file( fs::read_symlink( f ) );
            };

            auto simcase = fs::path( casename );

            if( exists( simcase ) ) {
                return simcase;
            }

            for( const auto& ext : { std::string("data"), std::string("DATA") } ) {
                if( exists( simcase.replace_extension( ext ) ) ) {
                    return simcase;
                }
            }

            throw std::invalid_argument( "Cannot find input case " + casename );
        }





        int64_t convertMessageType(const Message::type& mtype)
        {
            switch (mtype) {
            case Message::type::Debug:
                return Log::MessageType::Debug;
            case Message::type::Info:
                return Log::MessageType::Info;
            case Message::type::Warning:
                return Log::MessageType::Warning;
            case Message::type::Error:
                return Log::MessageType::Error;
            case Message::type::Problem:
                return Log::MessageType::Problem;
            case Message::type::Bug:
                return Log::MessageType::Bug;
            case Message::type::Note:
                return Log::MessageType::Note;
            }
            throw std::logic_error("Invalid messages type!\n");
        }


    } // namespace detail



} // namespace Opm

#endif // OPM_FLOWMAIN_HEADER_INCLUDED
