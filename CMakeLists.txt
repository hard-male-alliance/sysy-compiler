cmake_minimum_required(VERSION 3.25)

project(sysy_compiler VERSION 0.1.0 LANGUAGES CXX)

option(SYSY_ENABLE_SANITIZERS "Enable Linux AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(SYSY_ENABLE_SANITIZERS AND
   (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
    NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$"))
    message(FATAL_ERROR "SYSY_ENABLE_SANITIZERS requires Linux with GCC or Clang")
endif()

# 中文：仅为本项目目标启用检测，不污染依赖库或默认构建配置。
# English: Instrument project targets only; do not leak flags to dependencies or normal presets.
function(sysy_enable_sanitizers target)
    if(SYSY_ENABLE_SANITIZERS)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

# 中文：单体可执行文件统一承载编译器各模块，避免内部拆分影响命令行契约。
# English: Keep internal modules in one executable so refactoring cannot alter the CLI contract.
file(GLOB_RECURSE COMPILER_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/*.cpp")
if(NOT COMPILER_SOURCES)
    message(FATAL_ERROR "No compiler sources found under src/*.cpp")
endif()

add_executable(compiler ${COMPILER_SOURCES})
target_compile_features(compiler PRIVATE cxx_std_23)
set_target_properties(compiler PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(compiler PRIVATE "${PROJECT_SOURCE_DIR}/include")
sysy_enable_sanitizers(compiler)

# 中文：系统 SQLite 是持久化日志与追踪的唯一外部构建依赖。
# English: System SQLite is the sole external build dependency for durable logs and traces.
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
if(TARGET SQLite3::SQLite3)
    # 中文：CMake 4.3 起首选新名称，保留旧版 CMake 的兼容路径。
    # English: Prefer the CMake 4.3 target name while retaining older CMake support.
    set(SYSY_SQLITE_TARGET SQLite3::SQLite3)
else()
    set(SYSY_SQLITE_TARGET SQLite::SQLite3)
endif()
target_link_libraries(compiler PRIVATE ${SYSY_SQLITE_TARGET} Threads::Threads)

# 中文：目标运行时必须由链接最终程序的同一 GNU/Linux RISC-V 工具链构建；
# 预制归档依赖 Newlib，不能与 glibc 混链。该选项不影响宿主编译器构建。
# English: Build the target runtime with the same GNU/Linux RISC-V toolchain used
# for final programs; the bundled Newlib archive cannot link with glibc. This
# opt-in target never changes the host compiler build.
option(SYSY_BUILD_RISCV_RUNTIME "Build the GNU/Linux RISC-V SysY runtime archive" OFF)
if(SYSY_BUILD_RISCV_RUNTIME)
    find_program(SYSY_RISCV_CC NAMES riscv64-linux-gnu-gcc REQUIRED)
    find_program(SYSY_RISCV_AR NAMES riscv64-linux-gnu-ar REQUIRED)
    execute_process(
        COMMAND "${SYSY_RISCV_CC}" -dumpmachine
        OUTPUT_VARIABLE SYSY_RISCV_TRIPLE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE SYSY_RISCV_TRIPLE_STATUS)
    if(NOT SYSY_RISCV_TRIPLE_STATUS EQUAL 0 OR
       NOT SYSY_RISCV_TRIPLE MATCHES "^riscv64.*linux-gnu")
        message(FATAL_ERROR
            "SYSY_RISCV_CC must target riscv64 GNU/Linux, got '${SYSY_RISCV_TRIPLE}'")
    endif()

    set(SYSY_RUNTIME_DIR "${CMAKE_CURRENT_BINARY_DIR}/runtime/riscv64-linux-gnu")
    set(SYSY_RUNTIME_OBJECT "${SYSY_RUNTIME_DIR}/sylib.o")
    set(SYSY_RUNTIME_ARCHIVE "${SYSY_RUNTIME_DIR}/libsysy.a")
    add_custom_command(
        OUTPUT "${SYSY_RUNTIME_ARCHIVE}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SYSY_RUNTIME_DIR}"
        # 中文：上游头文件含暂定定义；-fcommon 保留原运行时的链接语义。
        # English: Upstream header has tentative definitions; -fcommon preserves
        # the supplied runtime's historical link semantics.
        COMMAND "${SYSY_RISCV_CC}" -std=c11 -O2 -fcommon
            -march=rv64gc -mabi=lp64d -c
            "${PROJECT_SOURCE_DIR}/libs/libsysy/sylib.c"
            -o "${SYSY_RUNTIME_OBJECT}"
        COMMAND "${SYSY_RISCV_AR}" rcs
            "${SYSY_RUNTIME_ARCHIVE}" "${SYSY_RUNTIME_OBJECT}"
        DEPENDS "${PROJECT_SOURCE_DIR}/libs/libsysy/sylib.c"
                "${PROJECT_SOURCE_DIR}/libs/libsysy/sylib.h"
        BYPRODUCTS "${SYSY_RUNTIME_OBJECT}"
        COMMENT "Building GNU/Linux RISC-V SysY runtime from supplied source"
        VERBATIM)
    add_custom_target(sysy_runtime_riscv ALL DEPENDS "${SYSY_RUNTIME_ARCHIVE}")
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    # 中文：独立测试 SSA 提升，避免仅凭端到端输出掩盖优化器内部错误。
    # English: Exercise SSA promotion independently so E2E outputs do not mask
    # optimizer-internal defects.
    add_executable(mem2reg_test tests/mem2reg_test.cpp src/optimize.cpp)
    target_compile_features(mem2reg_test PRIVATE cxx_std_23)
    set_target_properties(mem2reg_test PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(mem2reg_test PRIVATE "${PROJECT_SOURCE_DIR}/include")
    sysy_enable_sanitizers(mem2reg_test)
    add_test(NAME mem2reg_unit COMMAND mem2reg_test)
    add_executable(telemetry_test tests/telemetry_test.cpp src/telemetry.cpp)
    target_compile_features(telemetry_test PRIVATE cxx_std_23)
    set_target_properties(telemetry_test PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(telemetry_test PRIVATE "${PROJECT_SOURCE_DIR}/include")
    sysy_enable_sanitizers(telemetry_test)
    target_link_libraries(telemetry_test PRIVATE ${SYSY_SQLITE_TARGET} Threads::Threads)
    add_test(NAME telemetry_unit COMMAND telemetry_test)
    set_tests_properties(telemetry_unit PROPERTIES
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    if(EXISTS "${PROJECT_SOURCE_DIR}/tests/sysy_conformance.py")
        if(SYSY_BUILD_RISCV_RUNTIME)
            # 中文：目标测试必须执行，缺少 QEMU 不得伪装为通过。
            # English: Target execution is mandatory here; missing QEMU must
            # not produce a misleading passing test.
            set(SYSY_CONFORMANCE_TEST_NAME sysy_conformance_target)
            set(SYSY_CONFORMANCE_RUNTIME_ARGS
                --execute on --runtime-archive "${SYSY_RUNTIME_ARCHIVE}")
            message(STATUS
                "SysY target execution enabled: ${SYSY_RUNTIME_ARCHIVE} (requires QEMU at test time)")
        else()
            # 中文：名称揭示覆盖边界，缺少目标工具链时仍可验证编译路径。
            # English: Name the actual coverage boundary; host builds still
            # verify compilation without cross tools.
            set(SYSY_CONFORMANCE_TEST_NAME sysy_conformance_compile_only)
            set(SYSY_CONFORMANCE_RUNTIME_ARGS --execute off)
            message(STATUS
                "SysY target execution disabled; configure with -DSYSY_BUILD_RISCV_RUNTIME=ON to enable it")
        endif()
        add_test(NAME ${SYSY_CONFORMANCE_TEST_NAME}
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/sysy_conformance.py"
                --compiler "$<TARGET_FILE:compiler>"
                ${SYSY_CONFORMANCE_RUNTIME_ARGS})
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS compiler RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
