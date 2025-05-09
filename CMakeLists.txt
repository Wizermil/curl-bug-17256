cmake_minimum_required(VERSION 3.25)
project(curl_multi_crasher LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Sanitizer options
option(USE_ASAN "Enable Address Sanitizer with use-after-return detection" OFF)
option(USE_UBSAN "Enable Undefined Behavior Sanitizer" OFF)
option(USE_TSAN "Enable Thread Sanitizer" OFF)

# Sanitizer compatibility checks
if(USE_ASAN AND USE_TSAN)
  message(FATAL_ERROR "Cannot use both Address Sanitizer and Thread Sanitizer together")
endif()

# Configure sanitizer flags
if(USE_ASAN)
  message(STATUS "Enabling Address Sanitizer with use-after-return detection")
  set(ASAN_FLAGS "-fsanitize=address -fsanitize-address-use-after-return=always -fno-omit-frame-pointer")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${ASAN_FLAGS}")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${ASAN_FLAGS}")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")
endif()

if(USE_UBSAN)
  message(STATUS "Enabling Undefined Behavior Sanitizer")
  set(UBSAN_FLAGS "-fsanitize=undefined -fno-omit-frame-pointer")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${UBSAN_FLAGS}")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${UBSAN_FLAGS}")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=undefined")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=undefined")
endif()

if(USE_TSAN)
  message(STATUS "Enabling Thread Sanitizer")
  set(TSAN_FLAGS "-fsanitize=thread -fno-omit-frame-pointer")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${TSAN_FLAGS}")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${TSAN_FLAGS}")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=thread")
endif()

include(FetchContent)
option(USE_INTERNAL_CURL "Build curl with threaded resolver (no c-ares)" ON)

if(USE_INTERNAL_CURL)
  # Declare the source and desired build options BEFORE processing.
  FetchContent_Declare(curl_src
    GIT_REPOSITORY https://github.com/curl/curl.git
    GIT_TAG curl-8_13_0)

  # Set options that influence curl's own CMake when it is configured by
  # FetchContent. These must be cached *before* FetchContent_MakeAvailable.
  # Ensure DYLD symbol interposition works for all shared libs built in the
  # upcoming curl sub-project (must be set *before* FetchContent configures it)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(ENABLE_ARES OFF CACHE BOOL "" FORCE)
  set(ENABLE_THREADED_RESOLVER ON CACHE BOOL "" FORCE)
  set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
  set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
  set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
  set(CURL_USE_LIBSSH OFF CACHE BOOL "" FORCE)
  set(CURL_ENABLE_SSL ON CACHE BOOL "" FORCE)
  set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)

  FetchContent_MakeAvailable(curl_src)

  # FetchContent already defined target 'libcurl'; use it directly
  set(CURL_LIB_TARGET libcurl)
endif()

add_executable(crasher main.cpp)

# ---- fishhook static lib (local) ----
add_library(fishhook STATIC third_party/fishhook/fishhook.c)
target_include_directories(fishhook PUBLIC third_party/fishhook)
set_property(TARGET fishhook PROPERTY C_STANDARD 23)

# Objective-C++ interposer using fishhook
add_library(resolver_interpose SHARED fish_interposer.mm)
target_link_libraries(resolver_interpose PRIVATE fishhook)
set_target_properties(resolver_interpose PROPERTIES
  OUTPUT_NAME resolver_interpose
  CXX_STANDARD 23
  OBJCXX_STANDARD 23)

# Link against libcurl (threads come transitively)
set_property(TARGET crasher PROPERTY POSITION_INDEPENDENT_CODE ON)

# Link with selected curl target
target_link_libraries(crasher PRIVATE ${CURL_LIB_TARGET})
