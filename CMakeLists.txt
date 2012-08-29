# CMake build script for Velocity EQ bin plugin
# x64 Windows Server-only
# 2012/03/09 -- Steven.McCoy@thomsonreuters.com

cmake_minimum_required (VERSION 2.8.8)

project (Gomi)

set(GOMI_VERSION_MAJOR 2)
set(GOMI_VERSION_MINOR 3)
set(GOMI_VERSION_BUILD 20)

# Net-SNMP SDK installed, tested 5.7.1.
set(NETSNMP_ROOT D:/net-snmp-5.7.1)
set(NETSNMP_INCLUDE_DIRS
	${NETSNMP_ROOT}/include/net-snmp
	C:/usr/include
)
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(NETSNMP_BUILD_TYPE "debug")
else (CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(NETSNMP_BUILD_TYPE "release")
endif (CMAKE_BUILD_TYPE STREQUAL "Debug")
set(NETSNMP_LIBRARY_DIRS ${NETSNMP_ROOT}/win32/lib/${NETSNMP_BUILD_TYPE})
set(NETSNMP_LIBRARIES
	netsnmp.lib
	netsnmpagent.lib
	netsnmpmibs.lib
)

# TREP-VA 7.0 SDK
set(VHAYU_ROOT D:/Vhayu-7.0.5)
set(VHAYU_INCLUDE_DIRS
	${VHAYU_ROOT}/Engine/TBSDK/Include
	${VHAYU_ROOT}/Engine/ThirdParty/xerces-c-3.1.1-x86_64-windows-vc-10.0/include
)
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(VHAYU_BUILD_TYPE "debug")
	set(VHAYU_XERCES_LIBRARY "xerces-c_3D.lib")
else (CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(VHAYU_BUILD_TYPE "release")
	set(VHAYU_XERCES_LIBRARY "xerces-c_3.lib")
endif (CMAKE_BUILD_TYPE STREQUAL "Debug")
set(VHAYU_LIBRARY_DIRS
	${VHAYU_ROOT}/Engine/TBSDK/Lib64/${VHAYU_BUILD_TYPE}
	${VHAYU_ROOT}/Engine/ThirdParty/xerces-c-3.1.1-x86_64-windows-vc-10.0/lib
)
set(VHAYU_LIBRARIES
	FlexRecord.lib
	PluginFramework.lib
	Utility.lib
	SEDll.lib
	${VHAYU_XERCES_LIBRARY}
)

# Thomson Reuters Robust Foundation API
set(RFA_ROOT D:/rfa7.2.1.L1.win-shared.rrg)
set(RFA_INCLUDE_DIRS
	${RFA_ROOT}/Include
	${RFA_ROOT}/Include/rwf
)
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(RFA_BUILD_TYPE "Debug_MDd")
else (CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(RFA_BUILD_TYPE "Release_MD")
endif (CMAKE_BUILD_TYPE STREQUAL "Debug")
set(RFA_LIBRARY_DIRS ${RFA_ROOT}/Libs/WIN_64_VS100/${RFA_BUILD_TYPE})
set(RFA_LIBRARY_DIR ${RFA_LIBRARY_DIRS})
set(RFA_LIBRARIES
        RFA7_Common100_x64.lib
        RFA7_Config100_x64.lib
        RFA7_Logger100_x64.lib
        RFA7_Data100_x64.lib
        RFA7_Connections100_x64.lib
        RFA7_Connections_OMM100_x64.lib
        RFA7_SessionLayer100_x64.lib
)
set(RFA_RUNTIME_LIBRARIES
	${RFA_LIBRARY_DIR}/RFA7_Common100_x64.dll
	${RFA_LIBRARY_DIR}/RFA7_Config100_x64.dll
	${RFA_LIBRARY_DIR}/RFA7_Logger100_x64.dll
	${RFA_LIBRARY_DIR}/RFA7_Data100_x64.dll
	${RFA_LIBRARY_DIR}/RFA7_Connections100_x64.dll
	${RFA_LIBRARY_DIR}/RFA7_Connections_OMM100_x64.dll
	${RFA_LIBRARY_DIR}/RFA7_SessionLayer_OMM100_x64.dll
	${RFA_LIBRARY_DIR}/RFA7_SessionLayer100_x64.dll
	${RFA_LIBRARY_DIR}/RFA7_RSSL_Adapter100_x64.dll
	${RFA_LIBRARY_DIR}/RFA7_RSSL_Cons_Adapter100_x64.dll
	${RFA_LIBRARY_DIR}/RFA7_RSSL_Prov_Adapter100_x64.dll
)

# ZeroMQ SDK 2.2.0
set(ZEROMQ_ROOT "C:/Program Files/ZeroMQ 2.2.0")
set(ZEROMQ_INCLUDE_DIRS "${ZEROMQ_ROOT}/include")
set(ZEROMQ_LIBRARY_DIRS "${ZEROMQ_ROOT}/lib")
set(ZEROMQ_LIBRARIES
	optimized libzmq-v100-mt.lib
	debug libzmq-v100-mt-gd.lib
)
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(ZEROMQ_RUNTIME_LIBRARIES "${ZEROMQ_ROOT}/bin/libzmq-v100-mt-gd.dll")
else (CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(ZEROMQ_RUNTIME_LIBRARIES "${ZEROMQ_ROOT}/bin/libzmq-v100-mt.dll")
endif (CMAKE_BUILD_TYPE STREQUAL "Debug")

# Google Protocol BUffers
set(PROTOBUF_SRC_ROOT_FOLDER "D:/protobuf-2.4.1")
find_package (Protobuf REQUIRED)

# Boost headers plus built libraries
set(BOOST_ROOT D:/boost_1_51_0)
set(BOOST_LIBRARYDIR ${BOOST_ROOT}/stage/lib)
set(Boost_USE_STATIC_LIBS ON)
find_package (Boost 1.50 COMPONENTS chrono thread REQUIRED)

# Boost Atomics
set(Boost_INCLUDE_DIRS
	"D:/boost.atomic"
	${Boost_INCLUDE_DIRS}
)

# Python for source generators
find_package(PythonInterp REQUIRED)

#-----------------------------------------------------------------------------
# force off-tree build

if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
message(FATAL_ERROR "CMake generation is not allowed within the source directory!
Remove the CMakeCache.txt file and try again from another folder, e.g.:

   del CMakeCache.txt
   mkdir build
   cd build
   cmake ..
")
endif(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})

#-----------------------------------------------------------------------------
# default to Release build, frequently broken: manually run cmake-gui to fix

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING
      "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel."
      FORCE)
endif(NOT CMAKE_BUILD_TYPE)

set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/bin)
set(LIBRARY_OUTPUT_PATH  ${CMAKE_BINARY_DIR}/lib)

#-----------------------------------------------------------------------------
# platform specifics

add_definitions(
	-DWIN32
	-DWIN32_LEAN_AND_MEAN
# Windows Server 2008 (not-R2)
	-D_WIN32_WINNT=0x0600
# Net-SNMP Autoconf overrides
	-DHAVE_STDINT_H
# RFA version
        -DRFA_LIBRARY_VERSION="7.2.1."
# production release
	-DOFFICIAL_BUILD
)

# SEH Exceptions
set(CMAKE_CXX_FLAGS "/EHa")

#-----------------------------------------------------------------------------
# Generated source files

PROTOBUF_GENERATE_CPP(PROTO_SRCS PROTO_HDRS src/provider.proto)

configure_file(
	${CMAKE_SOURCE_DIR}/version_generator.py.in
	${CMAKE_BINARY_DIR}/version_generator.py
	@ONLY
)
add_custom_command(
	OUTPUT ${CMAKE_BINARY_DIR}/version.cc
	COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_BINARY_DIR}/version_generator.py > ${CMAKE_BINARY_DIR}/version.cc
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	DEPENDS ${CMAKE_BINARY_DIR}/version_generator.py
)
configure_file(
	${CMAKE_SOURCE_DIR}/src/version.hh
	${CMAKE_BINARY_DIR}/version.hh
	COPYONLY
)
configure_file(
	${CMAKE_SOURCE_DIR}/version.rc.in
	${CMAKE_BINARY_DIR}/version.rc
	@ONLY
)

#-----------------------------------------------------------------------------
# source files

set(cxx-sources
	src/gomi_bin.cc
	src/gomi_bar.cc
	src/config.cc
	src/error.cc
	src/plugin.cc
	src/provider.cc
	src/rfa.cc
	src/rfa_logging.cc
	src/client.cc
	src/snmp_agent.cc
	src/tcl.cc
	src/gomi.cc
	src/gomiMIB.cc
	src/chromium/chromium_switches.cc
	src/chromium/command_line.cc
	src/chromium/debug/stack_trace.cc
	src/chromium/debug/stack_trace_win.cc
	src/chromium/file_util.cc
	src/chromium/file_util_win.cc
	src/chromium/memory/singleton.cc
	src/chromium/metrics/histogram.cc
	src/chromium/metrics/stats_counters.cc
	src/chromium/metrics/stats_table.cc
	src/chromium/logging.cc
	src/chromium/shared_memory_win.cc
	src/chromium/string_piece.cc
	src/chromium/string_split.cc
	src/chromium/string_util.cc
	src/chromium/stringprintf.cc
	src/chromium/synchronization/lock.cc
	src/chromium/synchronization/lock_impl_win.cc
	src/chromium/vlog.cc
	src/googleurl/url_parse.cc
	${CMAKE_BINARY_DIR}/version.cc
)

set(rc-sources
	${CMAKE_BINARY_DIR}/version.rc
)

include_directories(
	include
	${CMAKE_CURRENT_BINARY_DIR}
	${NETSNMP_INCLUDE_DIRS}
	${VHAYU_INCLUDE_DIRS}
	${RFA_INCLUDE_DIRS}
	${ZEROMQ_INCLUDE_DIRS}
	${PROTOBUF_INCLUDE_DIRS}
	${Boost_INCLUDE_DIRS}
)

link_directories(
	${NETSNMP_LIBRARY_DIRS}
	${VHAYU_LIBRARY_DIRS}
	${RFA_LIBRARY_DIRS}
	${ZEROMQ_LIBRARY_DIRS}
	${Boost_LIBRARY_DIRS}
)

#-----------------------------------------------------------------------------
# output

add_library(Gomi SHARED ${cxx-sources} ${rc-sources} ${PROTO_SRCS} ${PROTO_HDRS})

target_link_libraries(Gomi
	${NETSNMP_LIBRARIES}
	${VHAYU_LIBRARIES}
	${RFA_LIBRARIES}
	${Boost_LIBRARIES}
	${ZEROMQ_LIBRARIES}
	${PROTOBUF_LIBRARIES}
	ws2_32.lib
	dbghelp.lib
)

set(config
	${CMAKE_CURRENT_SOURCE_DIR}/config/Gomi.xml
	${CMAKE_CURRENT_SOURCE_DIR}/config/example.reg
)
file(GLOB mibs "${CMAKE_CURRENT_SOURCE_DIR}/mibs/*.txt")

install (TARGETS Gomi DESTINATION bin)
install (FILES ${RFA_RUNTIME_LIBRARIES} DESTINATION bin)
install (FILES ${ZEROMQ_RUNTIME_LIBRARIES} DESTINATION bin)
install (FILES ${config} DESTINATION config)
install (FILES ${mibs} DESTINATION mibs)

set (CPACK_SOURCE_IGNORE_FILES "*.lib")

include (InstallRequiredSystemLibraries)
set (CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}")
set (CPACK_PACKAGE_DESCRIPTION_SUMMARY "Velocity Analytics EQ bin plugin")
set (CPACK_PACKAGE_VENDOR "Thomson Reuters")
set (CPACK_NSIS_CONTACT "Steven McCoy <Steven.McCoy@thomsonreuters.com>")
# There is a bug in NSI that does not handle full unix paths properly. Make
# sure there is at least one set of four (4) backlasshes.
set (CPACK_NSIS_MUI_ICON "${CMAKE_CURRENT_SOURCE_DIR}\\\\installer.ico")
set (CPACK_NSIS_MUI_UNIICON "${CMAKE_CURRENT_SOURCE_DIR}\\\\installer.ico")
set (CPACK_PACKAGE_ICON "${CMAKE_CURRENT_SOURCE_DIR}\\\\branding.bmp")
set (CPACK_NSIS_COMPRESSOR "/SOLID lzma")
set (CPACK_PACKAGE_VERSION_MAJOR "${GOMI_VERSION_MAJOR}")
set (CPACK_PACKAGE_VERSION_MINOR "${GOMI_VERSION_MINOR}")
set (CPACK_PACKAGE_VERSION_PATCH "${GOMI_VERSION_BUILD}")
include (CPack)

# end of file
