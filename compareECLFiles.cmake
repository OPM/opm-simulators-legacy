# This script manages the addition of tests.
# The tests are orchestrated by a shell script,
# configured using opm_set_test_driver()
# and then the appropriate helper macro is called to
# register the ctest entry through the opm_add_test macro.
# Information such as the binary to call and test tolerances
# are passed from the build system to the driver script through
# command line parameters. See the opm_add_test() documentation for
# details on the parameters passed to the macro.

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
function(add_test_compareECLFiles)
  set(oneValueArgs CASENAME FILENAME SIMULATOR ABS_TOL REL_TOL DIR DIR_PREFIX PREFIX)
  set(multiValueArgs TEST_ARGS)
  cmake_parse_arguments(PARAM "$" "${oneValueArgs}" "${multiValueArgs}" ${ARGN} )
  if(NOT PARAM_DIR)
    set(PARAM_DIR ${PARAM_CASENAME})
  endif()
  if(NOT PARAM_PREFIX)
    set(PARAM_PREFIX compareECLFiles)
  endif()
  set(RESULT_PATH ${BASE_RESULT_PATH}${PARAM_DIR_PREFIX}/${PARAM_SIMULATOR}+${PARAM_CASENAME})
  set(TEST_ARGS ${OPM_TESTS_ROOT}/${PARAM_DIR}/${PARAM_FILENAME} ${PARAM_TEST_ARGS})
  opm_add_test(${PARAM_PREFIX}_${PARAM_SIMULATOR}+${PARAM_FILENAME} NO_COMPILE
               EXE_NAME ${PARAM_SIMULATOR}
               DRIVER_ARGS ${OPM_TESTS_ROOT}/${PARAM_DIR} ${RESULT_PATH}
                           ${PROJECT_BINARY_DIR}/bin
                           ${PARAM_FILENAME}
                           ${PARAM_ABS_TOL} ${PARAM_REL_TOL}
                           ${COMPARE_ECL_COMMAND}
               TEST_ARGS ${TEST_ARGS})
endfunction()

###########################################################################
# TEST: add_test_compare_restarted_simulation
###########################################################################

# Input:
#   - casename: basename (no extension)
#
# Details:
#   - This test class compares the output from a restarted simulation
#     to that of a non-restarted simulation.
function(add_test_compare_restarted_simulation)
  set(oneValueArgs CASENAME FILENAME SIMULATOR ABS_TOL REL_TOL)
  set(multiValueArgs TEST_ARGS)
  cmake_parse_arguments(PARAM "$" "${oneValueArgs}" "${multiValueArgs}" ${ARGN} )

  set(RESULT_PATH ${BASE_RESULT_PATH}/restart/${PARAM_SIMULATOR}+${PARAM_CASENAME})
  set(TEST_ARGS ${OPM_TESTS_ROOT}/${PARAM_CASENAME}/${PARAM_FILENAME} ${PARAM_TEST_ARGS})

  opm_add_test(compareRestartedSim_${PARAM_SIMULATOR}+${PARAM_FILENAME} NO_COMPILE
               EXE_NAME ${PARAM_SIMULATOR}
               DRIVER_ARGS ${OPM_TESTS_ROOT}/${PARAM_CASENAME} ${RESULT_PATH}
                           ${PROJECT_BINARY_DIR}/bin
                           ${PARAM_FILENAME}
                           ${PARAM_ABS_TOL} ${PARAM_REL_TOL}
                           ${COMPARE_ECL_COMMAND}
                           ${OPM_PACK_COMMAND}
                           0
               TEST_ARGS ${TEST_ARGS})
endfunction()

if(NOT TARGET test-suite)
  add_custom_target(test-suite)
endif()

# Regression tests
opm_set_test_driver(${PROJECT_SOURCE_DIR}/tests/run-regressionTest.sh "")

# Set absolute tolerance to be used passed to the macros in the following tests
set(abs_tol 2e-2)
set(rel_tol 1e-5)
set(coarse_rel_tol 1e-2)

add_test_compareECLFiles(CASENAME spe1
                         FILENAME SPE1CASE2
                         SIMULATOR flow_legacy
                         ABS_TOL ${abs_tol}
                         REL_TOL ${coarse_rel_tol})

add_test_compareECLFiles(CASENAME spe1_2p
                         FILENAME SPE1CASE2_2P
                         SIMULATOR flow_legacy
                         ABS_TOL ${abs_tol}
                         REL_TOL ${coarse_rel_tol}
                         DIR spe1)

add_test_compareECLFiles(CASENAME spe1
                         FILENAME SPE1CASE1
                         SIMULATOR flow_sequential
                         ABS_TOL ${abs_tol}
                         REL_TOL ${rel_tol})


add_test_compareECLFiles(CASENAME spe3
                         FILENAME SPE3CASE1
                         SIMULATOR flow_legacy
                         ABS_TOL ${abs_tol}
                         REL_TOL ${coarse_rel_tol}
                         TEST_ARGS tolerance_wells=1e-6 max_iter=20)

add_test_compareECLFiles(CASENAME spe9
                         FILENAME SPE9_CP_SHORT
                         SIMULATOR flow_legacy
                         ABS_TOL ${abs_tol}
                         REL_TOL ${rel_tol})

# Restart tests
opm_set_test_driver(${PROJECT_SOURCE_DIR}/tests/run-restart-regressionTest.sh "")

# Cruder tolerances for the restarted tests
set(abs_tol_restart 2e-1)
set(rel_tol_restart 4e-5)
add_test_compare_restarted_simulation(CASENAME spe1
                                      FILENAME SPE1CASE2_ACTNUM
                                      SIMULATOR flow_legacy
                                      ABS_TOL ${abs_tol_restart}
                                      REL_TOL ${rel_tol_restart})

add_test_compare_restarted_simulation(CASENAME spe9
                                      FILENAME SPE9_CP_SHORT
                                      SIMULATOR flow_legacy
                                      ABS_TOL ${abs_tol_restart}
                                      REL_TOL ${rel_tol_restart})

# Init tests
opm_set_test_driver(${PROJECT_SOURCE_DIR}/tests/run-init-regressionTest.sh "")

add_test_compareECLFiles(CASENAME norne
                         FILENAME NORNE_ATW2013
                         SIMULATOR flow_legacy
                         ABS_TOL ${abs_tol}
                         REL_TOL ${rel_tol}
                         PREFIX compareECLInitFiles
                         DIR_PREFIX /init)
