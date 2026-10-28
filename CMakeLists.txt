cmake_minimum_required(VERSION 3.22)
project(nextgen_gui LANGUAGES CXX C VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)   # __int128
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(NG_ENABLE_LTO "Enable link-time optimisation for release builds" ON)
option(NG_BUILD_TESTS "Build the unit/integration test binary" ON)
option(NG_WARNINGS_AS_ERRORS "Treat warnings as errors for project sources" OFF)
# Portability: by default librdkafka and SQLite are built from the vendored sources and
# linked statically, and libstdc++/libgcc are linked statically. Only OpenSSL (and
# optionally zlib) come from the system.
option(NG_VENDORED_RDKAFKA "Build librdkafka from third_party/ and link statically" ON)
option(NG_VENDORED_SQLITE "Compile the vendored SQLite amalgamation" ON)
option(NG_STATIC_LIBSTDCXX "Link libstdc++ and libgcc statically" ON)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB)

include(CheckIPOSupported)
check_ipo_supported(RESULT NG_IPO_OK OUTPUT NG_IPO_MSG LANGUAGES CXX)

# ---------------------------------------------------------------------------
# librdkafka
# ---------------------------------------------------------------------------
if(NG_VENDORED_RDKAFKA)
  set(NG_RDKAFKA_VERSION 2.13.0)
  set(NG_RDKAFKA_SRC ${CMAKE_BINARY_DIR}/deps/librdkafka-${NG_RDKAFKA_VERSION})
  if(NOT EXISTS ${NG_RDKAFKA_SRC}/CMakeLists.txt)
    message(STATUS "Extracting vendored librdkafka ${NG_RDKAFKA_VERSION}")
    file(ARCHIVE_EXTRACT INPUT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/librdkafka-${NG_RDKAFKA_VERSION}.tar.gz
         DESTINATION ${CMAKE_BINARY_DIR}/deps)
  endif()
  set(RDKAFKA_BUILD_STATIC ON CACHE BOOL "" FORCE)
  set(RDKAFKA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(RDKAFKA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(WITH_SSL ON CACHE BOOL "" FORCE)
  set(WITH_SASL ON CACHE BOOL "" FORCE)
  set(WITH_SASL_CYRUS OFF CACHE BOOL "" FORCE)
  set(WITH_SASL_SCRAM ON CACHE BOOL "" FORCE)
  set(WITH_SASL_OAUTHBEARER ON CACHE BOOL "" FORCE)
  set(WITH_CURL OFF CACHE BOOL "" FORCE)
  set(WITH_ZSTD OFF CACHE BOOL "" FORCE)
  set(WITH_LIBDL OFF CACHE BOOL "" FORCE)
  set(WITH_PLUGINS OFF CACHE BOOL "" FORCE)
  set(ENABLE_LZ4_EXT OFF CACHE BOOL "" FORCE)
  set(ENABLE_DEVEL OFF CACHE BOOL "" FORCE)
  set(ENABLE_REFCNT_DEBUG OFF CACHE BOOL "" FORCE)
  if(ZLIB_FOUND)
    set(WITH_ZLIB ON CACHE BOOL "" FORCE)
  else()
    set(WITH_ZLIB OFF CACHE BOOL "" FORCE)
  endif()
  add_subdirectory(${NG_RDKAFKA_SRC} ${CMAKE_BINARY_DIR}/deps/librdkafka-build EXCLUDE_FROM_ALL)
  add_library(ng_rdkafka INTERFACE)
  target_link_libraries(ng_rdkafka INTERFACE rdkafka)
  target_include_directories(ng_rdkafka SYSTEM INTERFACE ${NG_RDKAFKA_SRC}/src)
  target_compile_definitions(ng_rdkafka INTERFACE LIBRDKAFKA_STATICLIB)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(RDKAFKA REQUIRED IMPORTED_TARGET rdkafka)
  add_library(ng_rdkafka INTERFACE)
  target_link_libraries(ng_rdkafka INTERFACE PkgConfig::RDKAFKA)
endif()

# ---------------------------------------------------------------------------
# SQLite
# ---------------------------------------------------------------------------
if(NG_VENDORED_SQLITE)
  add_library(ng_sqlite STATIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite/sqlite3.c)
  target_include_directories(ng_sqlite SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite)
  target_compile_definitions(ng_sqlite PRIVATE
    SQLITE_THREADSAFE=1 SQLITE_DQS=0 SQLITE_OMIT_LOAD_EXTENSION SQLITE_OMIT_DEPRECATED
    SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_LIKE_DOESNT_MATCH_BLOBS SQLITE_USE_ALLOCA
    SQLITE_OMIT_SHARED_CACHE SQLITE_ENABLE_JSON1 SQLITE_DEFAULT_FOREIGN_KEYS=0)
  target_compile_options(ng_sqlite PRIVATE -w)
  target_link_libraries(ng_sqlite PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
  add_library(ng_sqlite INTERFACE)
  target_link_libraries(ng_sqlite INTERFACE PkgConfig::SQLITE3)
endif()

# ---------------------------------------------------------------------------
# Header-only third-party libraries (vendored, see third_party/LICENSES.md)
# ---------------------------------------------------------------------------
add_library(ng_third_party INTERFACE)
target_include_directories(ng_third_party SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_definitions(ng_third_party INTERFACE
  CPPHTTPLIB_OPENSSL_SUPPORT
  CPPHTTPLIB_THREAD_POOL_COUNT=0
  CPPHTTPLIB_LISTEN_BACKLOG=1024
  JSON_USE_IMPLICIT_CONVERSIONS=0
  JSON_DIAGNOSTICS=0)
target_link_libraries(ng_third_party INTERFACE OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
if(ZLIB_FOUND)
  target_compile_definitions(ng_third_party INTERFACE CPPHTTPLIB_ZLIB_SUPPORT)
  target_link_libraries(ng_third_party INTERFACE ZLIB::ZLIB)
endif()

# ---------------------------------------------------------------------------
# Embedded web assets: everything under web/static is compiled into the binary
# ---------------------------------------------------------------------------
file(GLOB_RECURSE NG_WEB_ASSETS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/static/*)
set(NG_WEB_ASSETS_CPP ${CMAKE_CURRENT_BINARY_DIR}/generated/web_assets.cpp)
add_custom_command(
  OUTPUT ${NG_WEB_ASSETS_CPP}
  COMMAND ${CMAKE_COMMAND}
          -DNG_ASSET_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/web/static
          -DNG_OUTPUT=${NG_WEB_ASSETS_CPP}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_assets.cmake
  DEPENDS ${NG_WEB_ASSETS} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_assets.cmake
  COMMENT "Embedding web assets"
  VERBATIM)

# ---------------------------------------------------------------------------
# Library with everything except main()
# ---------------------------------------------------------------------------
file(GLOB_RECURSE NG_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(FILTER NG_SOURCES EXCLUDE REGEX "/src/main\\.cpp$")

add_library(nextgen_lib STATIC ${NG_SOURCES} ${NG_WEB_ASSETS_CPP})
target_include_directories(nextgen_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(nextgen_lib PUBLIC ng_third_party ng_rdkafka ng_sqlite Threads::Threads)
target_compile_options(nextgen_lib PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-omit-frame-pointer)
if(NG_WARNINGS_AS_ERRORS)
  target_compile_options(nextgen_lib PRIVATE -Werror)
endif()
target_compile_definitions(nextgen_lib PUBLIC NG_VERSION="${PROJECT_VERSION}")

add_executable(nextgen-gui src/main.cpp)
target_link_libraries(nextgen-gui PRIVATE nextgen_lib)
target_compile_options(nextgen-gui PRIVATE -Wall -Wextra -Wno-unused-parameter)
if(NG_STATIC_LIBSTDCXX)
  target_link_options(nextgen-gui PRIVATE -static-libstdc++ -static-libgcc)
endif()

if(NG_ENABLE_LTO AND NG_IPO_OK AND CMAKE_BUILD_TYPE STREQUAL "Release")
  set_property(TARGET nextgen_lib PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  set_property(TARGET nextgen-gui PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

install(TARGETS nextgen-gui RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(NG_BUILD_TOOLS "Build developer tools (algo oracle driver)" ON)
if(NG_BUILD_TOOLS)
  add_executable(nextgen-algo-driver tools/algo_oracle/driver.cpp)
  target_link_libraries(nextgen-algo-driver PRIVATE nextgen_lib)
  target_include_directories(nextgen-algo-driver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  if(NG_STATIC_LIBSTDCXX)
    target_link_options(nextgen-algo-driver PRIVATE -static-libstdc++ -static-libgcc)
  endif()
endif()

if(NG_BUILD_TESTS)
  enable_testing()
  file(GLOB_RECURSE NG_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
  add_executable(nextgen-tests ${NG_TEST_SOURCES})
  target_link_libraries(nextgen-tests PRIVATE nextgen_lib)
  target_include_directories(nextgen-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_options(nextgen-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
  if(NG_STATIC_LIBSTDCXX)
    target_link_options(nextgen-tests PRIVATE -static-libstdc++ -static-libgcc)
  endif()
  add_test(NAME unit COMMAND nextgen-tests)
endif()
