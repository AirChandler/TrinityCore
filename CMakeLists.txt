# Copyright (C) 2008-2016 TrinityCore <http://www.trinitycore.org/>
#
# This file is free software; as a special exception the author gives
# unlimited permission to copy and/or distribute it, with or without
# modifications, as long as this notice is preserved.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY, to the extent permitted by law; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

# We require CMake >= 3.22
cmake_minimum_required(VERSION 3.22.0)

# Set projectname (must be done AFTER setting configurationtypes)
project(TrinityCore)

# CMake policies (can not be handled elsewhere)
cmake_policy(SET CMP0005 OLD)
if(POLICY CMP0043)
  cmake_policy(SET CMP0043 OLD) # Disable 'Ignore COMPILE_DEFINITIONS_<Config> properties'
endif(POLICY CMP0043)

if(POLICY CMP0057)
  cmake_policy(SET CMP0057 NEW) # Support IN_LIST Operator required for BOOST
endif()

IF(POLICY CMP0077)
  cmake_policy(SET CMP0077 NEW)
ENDIF()

# add this options before PROJECT keyword
set(CMAKE_DISABLE_SOURCE_CHANGES ON)
set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)

# Set RPATH-handing (CMake parameters)
set(CMAKE_SKIP_BUILD_RPATH 0)
set(CMAKE_BUILD_WITH_INSTALL_RPATH 0)
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH 1)

# set macro-directory
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/macros")

# build in Release-mode by default if not explicitly set
if( NOT CMAKE_BUILD_TYPE )
  set(CMAKE_BUILD_TYPE "Release")
endif()

# Output directories
set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_LIST_DIR}/out/bin")
set(CONF_DIR "${CMAKE_CURRENT_LIST_DIR}/out/conf")
set(TOOLS ON)
set(USE_SCRIPTPCH OFF)
set(USE_COREPCH OFF)
set(WITH_COREDEBUG ON)
set(CMAKE_BUILD_TYPE "Debug")
IF(NOT WIN32)
  # Local copy should be installed in root directory (unix based systems)
  # TrinityCore requires OpenSSL v1.0.0 (differs from global install so point to local copy)
  set(OPENSSL_SSL_LIBRARIES "${CMAKE_CURRENT_LIST_DIR}/../openssl/openssl-1.0.2u/libssl.a")
  set(OPENSSL_CRYPTO_LIBRARIES "${CMAKE_CURRENT_LIST_DIR}/../openssl/openssl-1.0.2u/libcrypto.a")
  set(OPENSSL_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../openssl/openssl-1.0.2u/include")
  set(OPENSSL_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../openssl/openssl-1.0.2u/")
  # Point to custom install path for boost
  set(BOOST_ROOT "${CMAKE_CURRENT_LIST_DIR}/../boost_1_55_0" CACHE PATH "Boost library path")
  set(Boost_NO_SYSTEM_PATHS ON CACHE BOOL "Do not search system for Boost")
  set(CMAKE_C_COMPILER gcc)
  set(CMAKE_CXX_COMPILER g++)
ENDIF()

include(CheckCXXSourceRuns)
include(CheckIncludeFiles)

# set default buildoptions and print them
include(cmake/options.cmake)

# turn off PCH totally if enabled (hidden setting, mainly for devs)
if( NOPCH )
  set(USE_COREPCH 0)
  set(USE_SCRIPTPCH 0)
endif()

include(CheckPlatform)

include(GroupSources)
include(AutoCollect)

find_package(PCHSupport)
find_package(MySQL)

if(NOT WITHOUT_GIT)
  find_package(Git)
endif()

# Find revision ID and hash of the sourcetree
include(cmake/genrev.cmake)

# print out the results before continuing
include(cmake/showoptions.cmake)

# add dependencies
add_subdirectory(dep)

# add core sources
add_subdirectory(src)
