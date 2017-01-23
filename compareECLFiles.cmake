# This script manages the addition of tests.
# The tests are orchestrated by a shell script,
# configured using opm_set_test_driver()
# and then the appropriate helper macro is called to
# register the ctest entry through the opm_add_test macro.
# Information such as the binary to call and test tolerances
# are passed from the build system to the driver script through
# command line parameters. See the opm_add_test() documentation for
# details on the parameters passed to the macro.

# Set absolute tolerance to be used for testing
set(abs_tol 2e-2)
set(rel_tol 1e-5)

# Define some paths
set(BASE_RESULT_PATH ${PROJECT_BINARY_DIR}/tests/results)

###########################################################################
# TEST: compareECLFiles
###########################################################################

# Input:
#   - casename: basename (no extension)
#
# Details:
#   - This test class compares output from a simulation to reference files.
macro (add_test_compareECLFiles casename filename simulator prefix)

  set(RESULT_PATH ${BASE_RESULT_PATH}/${simulator}+${casename})
  opm_add_test(${prefix}_${simulator}+${filename} NO_COMPILE
               EXE_NAME ${simulator}
               DRIVER_ARGS ${OPM_DATA_ROOT}/${casename} ${RESULT_PATH}
                           ${CMAKE_BINARY_DIR}/bin
                           ${filename}
                           ${abs_tol} ${rel_tol}
                           ${COMPARE_SUMMARY_COMMAND}
                           ${COMPARE_ECL_COMMAND}
               TEST_ARGS ${OPM_DATA_ROOT}/${casename}/${filename}.DATA )
endmacro (add_test_compareECLFiles)

###########################################################################
# TEST: add_test_compare_restarted_simulation
###########################################################################

# Input:
#   - casename: basename (no extension)
#
# Details:
#   - This test class compares the output from a restarted simulation
#     to that of a non-restarted simulation.
macro (add_test_compare_restarted_simulation casename filename simulator)

  set(RESULT_PATH ${BASE_RESULT_PATH}/restart/${simulator}+${casename})
  opm_add_test(compareRestartedSim_${simulator}+${filename} NO_COMPILE
               EXE_NAME ${simulator}
               DRIVER_ARGS ${OPM_DATA_ROOT}/${casename} ${RESULT_PATH}
                           ${CMAKE_BINARY_DIR}/bin
                           ${filename}
                           ${abs_tol} ${rel_tol}
                           ${COMPARE_SUMMARY_COMMAND}
                           ${COMPARE_ECL_COMMAND}
               TEST_ARGS ${OPM_DATA_ROOT}/${casename}/${filename})
endmacro (add_test_compare_restarted_simulation)

###########################################################################
# TEST: add_test_compare_parallel_simulation
###########################################################################

# Input:
#   - casename: basename (no extension)
#
# Details:
#   - This test class compares the output from a parallel simulation
#     to the output from the serial instance of the same model.
macro (add_test_compare_parallel_simulation casename filename simulator)
  set(abs_tol 0.20)
  set(rel_tol 4e-4)
  set(RESULT_PATH ${BASE_RESULT_PATH}/parallel/${simulator}+${casename})

  # Add test that runs flow_mpi and outputs the results to file
  opm_add_test(compareParallelSim_${simulator}+${filename} NO_COMPILE
               EXE_NAME ${simulator}
               DRIVER_ARGS ${OPM_DATA_ROOT}/${casename} ${RESULT_PATH}
                           ${CMAKE_BINARY_DIR}/bin
                           ${filename}
                           ${abs_tol} ${rel_tol}
                           ${COMPARE_SUMMARY_COMMAND}
                           ${COMPARE_ECL_COMMAND}
               TEST_ARGS ${OPM_DATA_ROOT}/${casename}/${filename})
endmacro (add_test_compare_parallel_simulation)

if(NOT TARGET test-suite)
  add_custom_target(test-suite)
endif()

# Regression tests
opm_set_test_driver(${PROJECT_SOURCE_DIR}/tests/run-regressionTest.sh "")

add_test_compareECLFiles(spe1 SPE1CASE2 flow compareECLFiles)
add_test_compareECLFiles(spe1 SPE1CASE1 flow_sequential compareECLFiles)
add_test_compareECLFiles(spe3 SPE3CASE1 flow compareECLFiles)
add_test_compareECLFiles(spe9 SPE9_CP_SHORT flow compareECLFiles)

# Restart tests
opm_set_test_driver(${PROJECT_SOURCE_DIR}/tests/run-restart-regressionTest.sh "")

add_test_compare_restarted_simulation(spe1 SPE1CASE2_ACTNUM flow)
add_test_compare_restarted_simulation(spe9 SPE9_CP_SHORT flow)

# Init tests
opm_set_test_driver(${PROJECT_SOURCE_DIR}/tests/run-init-regressionTest.sh "")

add_test_compareECLFiles(norne NORNE_ATW2013 flow compareECLInitFiles)

# Parallel tests
if(MPI_FOUND)
  opm_set_test_driver(${PROJECT_SOURCE_DIR}/tests/run-parallel-regressionTest.sh "")

  add_test_compare_parallel_simulation(spe1 SPE1CASE2 flow_mpi)
  add_test_compare_parallel_simulation(spe3 SPE3CASE1 flow_mpi)
  add_test_compare_parallel_simulation(spe9 SPE9_CP_SHORT flow_mpi)
endif()
