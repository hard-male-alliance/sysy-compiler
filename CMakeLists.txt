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

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
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
        add_test(NAME sysy_conformance
            COMMAND "${Python3_EXECUTABLE}"
                "${PROJECT_SOURCE_DIR}/tests/sysy_conformance.py"
                --compiler "$<TARGET_FILE:compiler>")
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS compiler RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
