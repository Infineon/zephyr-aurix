include(${ZEPHYR_BASE}/cmake/compiler/gcc/compiler_flags.cmake)

set_property(TARGET compiler-cpp PROPERTY dialect_cpp2a "-std=c++2a"
  "-Wno-register" "-Wno-deprecated-volatile")
set_property(TARGET compiler-cpp PROPERTY dialect_cpp20 "-std=c++20"
  "-Wno-register" "-Wno-deprecated-volatile")
set_property(TARGET compiler-cpp PROPERTY dialect_cpp2b "-std=c++2b"
  "-Wno-register" "-Wno-deprecated-volatile")
set_property(TARGET compiler-cpp PROPERTY dialect_cpp23 "-std=c++23"
  "-Wno-register" "-Wno-deprecated-volatile")

set_compiler_property(PROPERTY optimization_fast -O3 -ffast-math)

check_set_compiler_property(PROPERTY warning_base
                            -Wall
                            -Wformat
                            -Wformat-security
                            -Wno-format-zero-length
                            -Wno-unused-but-set-variable
                            -Wno-typedef-redefinition
                            -Wno-deprecated-non-prototype
)

check_set_compiler_property(APPEND PROPERTY warning_base -Wdouble-promotion)

check_set_compiler_property(APPEND PROPERTY warning_base -Wno-pointer-sign)

check_set_compiler_property(APPEND PROPERTY warning_base -Wpointer-arith)

set_compiler_property(PROPERTY warning_dw_1
                      -Wextra
                      -Wunused
                      -Wno-unused-parameter
                      -Wmissing-declarations
                      -Wmissing-format-attribute
)
check_set_compiler_property(APPEND PROPERTY warning_dw_1
                            -Wold-style-definition
                            -Wmissing-prototypes
                            -Wmissing-include-dirs
                            -Wunused-but-set-variable
                            -Wno-missing-field-initializers
)

set_compiler_property(PROPERTY warning_dw_2
                      -Waggregate-return
                      -Wcast-align
                      -Wdisabled-optimization
                      -Wnested-externs
                      -Wshadow
)

check_set_compiler_property(APPEND PROPERTY warning_dw_2
                            -Wlogical-op
                            -Wmissing-field-initializers
)

set_compiler_property(PROPERTY warning_dw_3
                      -Wbad-function-cast
                      -Wcast-qual
                      -Wconversion
                      -Wpacked
                      -Wpadded
                      -Wpointer-arith
                      -Wredundant-decls
                      -Wswitch-default
)

check_set_compiler_property(APPEND PROPERTY warning_dw_3
                            -Wpacked-bitfield-compat
                            -Wvla
)

check_set_compiler_property(PROPERTY warning_extended
                            -Wno-self-assign
                            -Wno-initializer-overrides
                            -Wno-section
                            -Wno-gnu
)

set_compiler_property(PROPERTY warning_error_coding_guideline
                      -Werror=vla
                      -Wimplicit-fallthrough
                      -Wconversion
                      -Woverride-init
)

set_compiler_property(PROPERTY no_printf_return_value)

set_property(TARGET compiler-cpp PROPERTY dialect_cpp2a "-std=c++2a" "-Wno-register")
set_property(TARGET compiler-cpp PROPERTY dialect_cpp20 "-std=c++20" "-Wno-register")
set_property(TARGET compiler-cpp PROPERTY dialect_cpp2b "-std=c++2b" "-Wno-register")

if(CONFIG_COVERAGE_NATIVE_SOURCE)
  set_compiler_property(PROPERTY coverage -fprofile-instr-generate -fcoverage-mapping)
else()
  set_compiler_property(PROPERTY coverage --coverage -fno-inline)
endif()

set_compiler_property(PROPERTY security_fortify_compile_time)
set_compiler_property(PROPERTY security_fortify_run_time)

check_set_compiler_property(PROPERTY hosted)

set_compiler_property(PROPERTY save_temps -save-temps)

set_compiler_property(PROPERTY linker_script -Wl,-T)

set_compiler_property(PROPERTY diagnostic -fcolor-diagnostics)

set_compiler_property(PROPERTY no_track_macro_expansion "-fmacro-backtrace-limit=1")

if(CONFIG_TRICORE)
  set_compiler_property(PROPERTY no_global_merge "")
else()
  set_compiler_property(PROPERTY no_global_merge "-mno-global-merge")
endif()

set_compiler_property(PROPERTY specs)
