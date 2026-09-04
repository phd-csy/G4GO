if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED BIN2C)
    message(FATAL_ERROR "INPUT, OUTPUT and BIN2C are required")
endif()

execute_process(
    COMMAND "${BIN2C}" --const --length --stdint --name g4go_optix_ir
            "${INPUT}"
    OUTPUT_FILE "${OUTPUT}"
    RESULT_VARIABLE exit_code
    ERROR_VARIABLE error_output
)

if(NOT exit_code EQUAL 0)
    message(FATAL_ERROR "bin2c failed: ${error_output}")
endif()
