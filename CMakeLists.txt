cmake_minimum_required(VERSION 2.8 FATAL_ERROR)

# must be done BEFORE project in order to work!
include(cmake/install_prefix.cmake)

# Project globals:
project( StrusPattern )
set( STRUS_MAJOR_VERSION 0 )
set( STRUS_MINOR_VERSION 15 )
set( STRUS_PATCH_VERSION 0 )
set( STRUS_VERSION ${STRUS_MAJOR_VERSION}.${STRUS_MINOR_VERSION}.${STRUS_PATCH_VERSION} )
set( CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake" )

if(NOT STRUS_ALL)
include( cmake/dist.cmake )
include( cmake/LinuxDistribution.cmake )
include( cmake/build_rules.cmake )
include( cmake/boost.cmake )
include( cmake/intl.cmake )
include( cmake/standard_targets.cmake )
include( cmake/find_strus_package.cmake )
include( cmake/find_tre.cmake )
endif()

find_strus_package( 3rdParty )
find_strus_package( base )
find_strus_package( analyzer )
find_strus_package( module )

configure_file( "${PROJECT_SOURCE_DIR}/include/strus/versionPattern.hpp.in"  "${CMAKE_BINARY_DIR}/include/strus/versionPattern.hpp"  @ONLY )
configure_file( "${PROJECT_SOURCE_DIR}/dist/obs/deploy_to_obs.sh.in"  "${PROJECT_SOURCE_DIR}/dist/obs/deploy_to_obs.sh"  @ONLY )
configure_file( "${PROJECT_SOURCE_DIR}/dist/archlinux/PKGBUILD.in"  "${PROJECT_SOURCE_DIR}/dist/archlinux/PKGBUILD"  @ONLY )
configure_file( "${PROJECT_SOURCE_DIR}/dist/obs/struspattern.dsc.in"  "${PROJECT_SOURCE_DIR}/dist/obs/struspattern.dsc"  @ONLY )
configure_file( "${PROJECT_SOURCE_DIR}/dist/redhat/struspattern.spec.in"  "${PROJECT_SOURCE_DIR}/dist/redhat/struspattern.spec"  @ONLY )

set( STRUS_GETTEXT_PACKAGE "struspattern-dom" )	#... 1st parameter of bindtextdomain(...)
set( STRUS_GETTEXT_LOCALEDIR "" )		   	  #... 2nd parameter of bindtextdomain(...)
configure_file( "${PROJECT_SOURCE_DIR}/src/internationalization.cpp.in"  "${PROJECT_SOURCE_DIR}/src/internationalization.cpp"  @ONLY )

enable_testing()

# Subdirectories:
add_subdirectory( 3rdParty )
add_subdirectory( src )
add_subdirectory( tests )
  
if(NOT STRUS_ALL)
include( cmake/report_build_settings.cmake )
endif()

# Install header files:
install(DIRECTORY include/strus
          DESTINATION include
          FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY include/strus
          DESTINATION include
          FILES_MATCHING PATTERN "*.h")

