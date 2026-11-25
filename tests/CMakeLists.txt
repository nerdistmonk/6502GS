add_test(NAME asm816_version
    COMMAND asm816 --version)
set_tests_properties(asm816_version PROPERTIES
    PASS_REGULAR_EXPRESSION "^asm816 0\\.0\\.1")

set(MERLIN32_BIN "" CACHE FILEPATH "Path to merlin32 binary, enables oracle tests")
set(MERLIN32_LIB "" CACHE PATH "Merlin32 macro library directory, required with MERLIN32_BIN")

if(MERLIN32_BIN)
    file(GLOB ORACLE_CASES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/oracle/cases/*.s)
    foreach(case_file IN LISTS ORACLE_CASES)
        get_filename_component(case_name ${case_file} NAME_WE)
        add_test(NAME oracle_${case_name}
            COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/oracle/run_oracle.sh
                $<TARGET_FILE:asm816> ${MERLIN32_BIN} ${MERLIN32_LIB} ${case_file})
    endforeach()

    file(GLOB SYMBOL_CASES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/oracle/symbol_cases/*.s)
    foreach(case_file IN LISTS SYMBOL_CASES)
        get_filename_component(case_name ${case_file} NAME_WE)
        add_test(NAME oracle_${case_name}
            COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/oracle/run_symbols.sh
                $<TARGET_FILE:asm816> ${MERLIN32_BIN} ${MERLIN32_LIB} ${case_file})
    endforeach()

    file(GLOB ERROR_CASES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/oracle/error_cases/*.s)
    foreach(case_file IN LISTS ERROR_CASES)
        get_filename_component(case_name ${case_file} NAME_WE)
        add_test(NAME oracle_error_${case_name}
            COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/oracle/run_errors.sh
                $<TARGET_FILE:asm816> ${MERLIN32_BIN} ${MERLIN32_LIB} ${case_file})
    endforeach()
endif()
