cmake_minimum_required(VERSION 3.22.1)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
	set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM} CACHE STRING "Compiler launcher for C.")
	set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM} CACHE STRING "Compiler launcher for CXX.")
endif()

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/shell/cmake")

project(flycast)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(DetectArchitecture)
if(NOT DEFINED ARCHITECTURE)
	message(FATAL_ERROR "Unsupported architecture encountered. Ending CMake generation.")
endif()
message(STATUS "Target architecture: ${ARCHITECTURE}")

if(VITA)
	include("${VITASDK}/share/vita.cmake" REQUIRED)
	set(VITA_DEPS_PREFIX "" CACHE PATH "Prefix of the vitaGL and SDL2 (vitagl backend) builds if they aren't installed in the VitaSDK")
	# Not available or not wanted on the Vita
	set(USE_OPENMP OFF)
	set(USE_ALSA OFF)
	set(USE_LIBAO OFF)
	set(USE_PULSEAUDIO OFF)
	set(USE_LUA OFF)
	set(USE_HOST_LIBZIP OFF)
	add_compile_definitions(_GNU_SOURCE)
	if(VITA_DEPS_PREFIX)
		set(SDL2_DIR "${VITA_DEPS_PREFIX}/lib/cmake/SDL2")
	endif()
endif()

option(ENABLE_CTEST "Enables unit tests" OFF)
option(ENABLE_OPROFILE "Enable OProfile" OFF)
option(TEST_AUTOMATION "Enable test automation" OFF)
option(ENABLE_LOG "Enable full logging" OFF)
option(ASAN "Enable address sanitizer" OFF)
option(USE_GLES "Use GLES[3] API" OFF)
option(USE_GLES2 "Use GLES2 API" OFF)
option(USE_HOST_LIBZIP "Use host libzip" ON)
option(USE_HOST_LIBCHDR "Use host libchdr" OFF)
option(USE_OPENMP "Use OpenMP if available" ON)
option(USE_OPENGL "Use OpenGL API" ON)
option(USE_ALSA "Build with ALSA support" ON)
option(USE_LIBAO "Build with AO support" ON)
option(USE_OSS "Build with OSS support" OFF)
option(USE_PULSEAUDIO "Build with PulseAudio support" ON)
option(USE_LUA "Build with Lua support" ON)
option(ENABLE_GDB_SERVER "Build with GDB debugging support" OFF)
option(ENABLE_DC_PROFILER "Build with support for target machine (SH4) profiler" OFF)
option(ENABLE_FC_PROFILER "Build with support for host app (Flycast) profiler" OFF)
option(USE_LIBCDIO "Use libcdio for CDROM access" OFF)

include(GNUInstallDirs)
include(CMakeRC)

if(ENABLE_CTEST)
	include(CTest)
	add_compile_definitions(FLYCAST_TEST_FILES="${CMAKE_CURRENT_SOURCE_DIR}/tests/files")
endif()

find_package(Git)
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
	execute_process(
		COMMAND ${GIT_EXECUTABLE} describe --tags --always
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		OUTPUT_VARIABLE GIT_VERSION
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	execute_process(
		COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		OUTPUT_VARIABLE GIT_HASH
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
else()
	set(GIT_VERSION "v0.0.0-0-g000000000")
	set(GIT_HASH "000000000")
endif()

string(TIMESTAMP BUILD_TIMESTAMP UTC)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/core/version.h.in" "${CMAKE_CURRENT_SOURCE_DIR}/core/version.h" @ONLY)

add_executable(${PROJECT_NAME} core/emulator.cpp)

set_target_properties(${PROJECT_NAME} PROPERTIES
		CMAKE_CXX_STANDARD 20
		CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT VITA)
	# Vita executables need their relocations
	set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS_RELEASE -s)
endif()
target_compile_options(${PROJECT_NAME} PRIVATE
	$<$<COMPILE_LANGUAGE:CXX>:-fno-strict-aliasing>
	$<$<COMPILE_LANGUAGE:CXX>:-Wall>)

target_compile_definitions(${PROJECT_NAME} PRIVATE
		$<$<BOOL:${ENABLE_LOG}>:DEBUGFAST>
		$<$<BOOL:${TEST_AUTOMATION}>:TEST_AUTOMATION>)

if(UNIX)
	execute_process(COMMAND getconf PAGESIZE OUTPUT_VARIABLE PAGE_SIZE OUTPUT_STRIP_TRAILING_WHITESPACE)
	target_compile_definitions(${PROJECT_NAME} PRIVATE PAGE_SIZE=${PAGE_SIZE})
endif()

target_include_directories(${PROJECT_NAME} PRIVATE core core/deps core/deps/stb core/deps/json core/deps/asio/asio/include)

if(USE_OPENMP)
	find_package(OpenMP)
	if(OpenMP_CXX_FOUND)
		target_link_libraries(${PROJECT_NAME} PRIVATE OpenMP::OpenMP_CXX)
	endif()
endif()

option(BUILD_SHARED_LIBS "Build shared library" OFF)
set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "Build the xxhsum binary")
add_subdirectory(core/deps/xxHash/cmake_unofficial)
target_link_libraries(${PROJECT_NAME} PRIVATE xxHash::xxhash)

option(BUILD_SHARED_LIBS "Build shared library" OFF)
add_subdirectory(core/deps/glm)
target_link_libraries(${PROJECT_NAME} PRIVATE glm::glm)

if(USE_OSS)
	target_compile_definitions(${PROJECT_NAME} PRIVATE USE_OSS)
endif()

if(USE_ALSA)
	find_package(ALSA)
	if(ALSA_FOUND)
		target_compile_definitions(${PROJECT_NAME} PRIVATE USE_ALSA)
		target_include_directories(${PROJECT_NAME} PRIVATE ${ALSA_INCLUDE_DIRS})
		target_link_libraries(${PROJECT_NAME} PRIVATE ${ALSA_LIBRARIES})
	endif()
endif()

find_package(SDL2 2.0.18)
if(SDL2_FOUND)
	if(TARGET SDL2::SDL2main AND NOT BUILD_TESTING AND NOT VITA)
		target_link_libraries(${PROJECT_NAME} PRIVATE SDL2::SDL2main)
	endif()

	if(TARGET SDL2::SDL2)
		target_link_libraries(${PROJECT_NAME} PRIVATE SDL2::SDL2)
	else()
		target_include_directories(${PROJECT_NAME} PRIVATE ${SDL2_INCLUDE_DIRS})
		target_link_libraries(${PROJECT_NAME} PRIVATE ${SDL2_LIBRARIES})
	endif()

	target_compile_definitions(${PROJECT_NAME} PRIVATE USE_SDL USE_SDL_AUDIO)
	add_subdirectory(core/sdl)
endif()

if(VITA)
	# The VitaSDK CURL package references the SDK build location
	target_compile_definitions(${PROJECT_NAME} PRIVATE CURL_STATICLIB)
	target_link_libraries(${PROJECT_NAME} PRIVATE curl ssl crypto zstd z)
else()
	find_package(CURL REQUIRED)
	target_link_libraries(${PROJECT_NAME} PRIVATE CURL::libcurl)
endif()

find_package(ZLIB)
if(TARGET ZLIB::ZLIB)
	set(WITH_SYSTEM_ZLIB ON CACHE BOOL "Use system provided zlib library")
	target_link_libraries(${PROJECT_NAME} PRIVATE ZLIB::ZLIB)
endif()

if(USE_LUA)
	find_package(Lua 5.2)
	if(LUA_FOUND)
		target_compile_definitions(${PROJECT_NAME} PRIVATE USE_LUA)
		target_include_directories(${PROJECT_NAME} PRIVATE ${LUA_INCLUDE_DIR} core/deps/luabridge/Source)
		target_link_libraries(${PROJECT_NAME} PRIVATE ${LUA_LIBRARIES})
	endif()
endif()

find_package(PkgConfig)

if(NOT WITH_SYSTEM_ZLIB)
    option(ZLIB_BUILD_EXAMPLES "Enable Zlib Examples" OFF)
    add_subdirectory("core/deps/libchdr/deps/zlib-1.3.1" EXCLUDE_FROM_ALL)
	set_target_properties(zlibstatic PROPERTIES POSITION_INDEPENDENT_CODE ON)
	target_link_libraries(${PROJECT_NAME} PRIVATE zlibstatic)
	# help libzip find the package
	set(ZLIB_RELATIVE_PATH "core/deps/libchdr/deps/zlib-1.3.1")
	set(ZLIB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${ZLIB_RELATIVE_PATH}" "${CMAKE_CURRENT_BINARY_DIR}/${ZLIB_RELATIVE_PATH}")
endif()

if(PKG_CONFIG_FOUND AND USE_HOST_LIBCHDR)
	pkg_check_modules(LIBCHDR IMPORTED_TARGET libchdr)
	target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::LIBCHDR)
else()
	option(ZSTD_BUILD_SHARED "BUILD SHARED LIBRARIES" OFF)
	option(ZSTD_BUILD_PROGRAMS "BUILD PROGRAMS" OFF)
	option(ZSTD_LEGACY_SUPPORT "LEGACY SUPPORT" OFF)
	add_subdirectory(core/deps/libchdr/deps/zstd-1.5.6/build/cmake EXCLUDE_FROM_ALL)
	target_link_libraries(${PROJECT_NAME} PRIVATE libzstd_static)

	option(WITH_SYSTEM_ZSTD "Use system provided zstd library" ON)
	add_subdirectory(core/deps/libchdr EXCLUDE_FROM_ALL)
	target_link_libraries(${PROJECT_NAME} PRIVATE chdr-static)
	if(VITA)
		# vita-elf-create can't handle position-independent code relocations
		set_target_properties(libzstd_static lzma PROPERTIES POSITION_INDEPENDENT_CODE OFF)
	endif()
	target_include_directories(${PROJECT_NAME} PRIVATE core/deps/libchdr/include)
endif()

if(PKG_CONFIG_FOUND)
	if(USE_LIBAO)
		pkg_check_modules(AO IMPORTED_TARGET ao)
		if(AO_FOUND)
			target_compile_definitions(${PROJECT_NAME} PRIVATE USE_LIBAO)
			target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::AO)
		endif()
	endif()

	if(NOT SDL2_FOUND)
		pkg_check_modules(LIBEVDEV IMPORTED_TARGET libevdev)
		if(LIBEVDEV_FOUND)
			target_compile_definitions(${PROJECT_NAME} PRIVATE USE_EVDEV)
			target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::LIBEVDEV)

			pkg_check_modules(LIBUDEV IMPORTED_TARGET libudev)
			if(LIBUDEV_FOUND)
				target_compile_definitions(${PROJECT_NAME} PRIVATE USE_UDEV)
				target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::LIBUDEV)
			endif()
		endif()
	endif()

	if(USE_PULSEAUDIO)
		pkg_check_modules(LIBPULSE IMPORTED_TARGET libpulse)
		if(LIBPULSE_FOUND)
			target_compile_definitions(${PROJECT_NAME} PRIVATE USE_PULSEAUDIO)
			target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::LIBPULSE)
		endif()
	endif()

	if(USE_HOST_LIBZIP)
		pkg_check_modules(LIBZIP IMPORTED_TARGET libzip)
		if(LIBZIP_FOUND)
			target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::LIBZIP)
		endif()
	endif()

	if(ENABLE_OPROFILE)
		target_compile_definitions(${PROJECT_NAME} PRIVATE DYNA_OPROF)
		target_link_libraries(${PROJECT_NAME} PRIVATE opagent)
	endif()

	find_package(MiniUPnPc)
	if(MINIUPNP_FOUND)
		target_include_directories(${PROJECT_NAME} PRIVATE ${MINIUPNP_INCLUDE_DIRS})
		target_link_libraries(${PROJECT_NAME} PRIVATE ${MINIUPNP_LIBRARIES})
	endif()
endif()

if(UNIX)
	add_definitions(
		-DFLYCAST_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/${PROJECT_NAME}/"
		-DFLYCAST_SYSCONFDIR="${CMAKE_INSTALL_FULL_SYSCONFDIR}/${PROJECT_NAME}/"
	)

	# Desktop OpenGL 4 is not supported: OpenGL builds use the GLES2 or GLES3 API
	if(USE_GLES)
		target_compile_definitions(${PROJECT_NAME} PRIVATE GLES GLES3)
	elseif(USE_OPENGL)
		target_compile_definitions(${PROJECT_NAME} PRIVATE GLES GLES2)
	endif()

	find_package(Threads REQUIRED)
	target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)

	if(NOT SDL2_FOUND)
		find_package(X11)
		if(X11_FOUND)
			target_compile_definitions(${PROJECT_NAME} PRIVATE SUPPORT_X11)
			target_include_directories(${PROJECT_NAME} PRIVATE ${X11_INCLUDE_DIR})
			target_link_libraries(${PROJECT_NAME} PRIVATE ${X11_LIBRARIES})
		endif()
	endif()

	find_library(LIBRT rt)
	if(LIBRT)
		target_link_libraries(${PROJECT_NAME} PRIVATE ${LIBRT})
	endif()

	target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS})
endif()

if(VITA)
	target_compile_definitions(${PROJECT_NAME} PRIVATE GLES GLES2 ASIO_DISABLE_SERIAL_PORT ASIO_DISABLE_LOCAL_SOCKETS
		IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
	# The ARM dynarecs call C++ code with BL: it must not be Thumb code
	target_compile_options(${PROJECT_NAME} PRIVATE -marm)
	target_include_directories(${PROJECT_NAME} BEFORE PRIVATE core/vita/include)
	if(VITA_DEPS_PREFIX)
		target_include_directories(${PROJECT_NAME} BEFORE PRIVATE "${VITA_DEPS_PREFIX}/include")
		target_link_directories(${PROJECT_NAME} BEFORE PRIVATE "${VITA_DEPS_PREFIX}/lib")
	endif()
	target_link_libraries(${PROJECT_NAME} PRIVATE
		vitaGL
		vitashark
		SceShaccCg_stub
		SceShaccCgExt
		mathneon
		taihen_stub
		kubridge_stub_weak
		SceAppMgr_stub
		SceAppUtil_stub
		SceAudio_stub
		SceAudioIn_stub
		SceCommonDialog_stub
		SceCtrl_stub
		SceDisplay_stub
		SceGxm_stub
		SceHid_stub
		SceIme_stub
		SceKernelDmacMgr_stub
		SceLibKernel_stub
		SceMotion_stub
		SceNet_stub
		SceNetCtl_stub
		ScePgf_stub
		ScePower_stub
		SceSysmodule_stub
		SceTouch_stub
		SceVshBridge_stub
		# libstdc++ only enables std::thread if all of pthread is linked
		-Wl,--whole-archive pthread -Wl,--no-whole-archive)
	target_sources(${PROJECT_NAME} PRIVATE
		core/linux/common.cpp
		core/linux/unwind_info.cpp
		core/vita/main.cpp
		core/vita/posix_compat.cpp
		core/vita/vita_vmem.cpp)
endif()

if(ASAN)
	target_compile_options(${PROJECT_NAME} PRIVATE -fsanitize=address -static-libasan)
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address -static-libasan")
endif()

target_sources(${PROJECT_NAME} PRIVATE
		core/deps/chdpsr/cdipsr.cpp
		core/deps/chdpsr/cdipsr.h)

add_subdirectory(core/deps/nowide EXCLUDE_FROM_ALL)
target_link_libraries(${PROJECT_NAME} PRIVATE nowide::nowide)

if(VITA)
	target_compile_definitions(${PROJECT_NAME} PRIVATE FEAT_NO_MINIUPNPC)
elseif(NOT MINIUPNP_FOUND)
	option(UPNPC_BUILD_SHARED "Build shared library" OFF)
	option(UPNPC_BUILD_TESTS "Build test executables" OFF)
	option(UPNPC_BUILD_SAMPLE "Build sample executables" OFF)
	option(UPNPC_NO_INSTALL "Disable installation" ON)
	add_subdirectory(core/deps/miniupnpc)
	target_link_libraries(${PROJECT_NAME} PRIVATE miniupnpc::miniupnpc)
endif()

if(NOT LIBZIP_FOUND)
	option(ENABLE_COMMONCRYPTO "Enable use of CommonCrypto" OFF)
	option(ENABLE_GNUTLS "Enable use of GnuTLS" OFF)
	option(ENABLE_MBEDTLS "Enable use of mbed TLS" OFF)
	option(ENABLE_OPENSSL "Enable use of OpenSSL" OFF)
	option(ENABLE_WINDOWS_CRYPTO "Enable use of Windows cryptography libraries" OFF)
	option(ENABLE_BZIP2 "Enable use of BZip2" OFF)
	option(ENABLE_LZMA "Enable use of LZMA" OFF)
	option(ENABLE_ZSTD "Enable use of Zstandard" ON)
	option(BUILD_TOOLS "Build tools in the src directory (zipcmp, zipmerge, ziptool)" OFF)
	option(BUILD_REGRESS "Build regression tests" OFF)
	option(BUILD_EXAMPLES "Build examples" OFF)
	option(BUILD_DOC "Build documentation" OFF)
	option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
	option(LIBZIP_DO_INSTALL "Install libzip and the related files" OFF)
	add_subdirectory(core/deps/libzip EXCLUDE_FROM_ALL)
	target_link_libraries(${PROJECT_NAME} PRIVATE libzip::zip)
endif()

if(USE_LIBCDIO)
	if(PKG_CONFIG_FOUND)
		pkg_check_modules(CDIO IMPORTED_TARGET libcdio)
		if(CDIO_FOUND)
			target_compile_definitions(${PROJECT_NAME} PRIVATE USE_LIBCDIO)
			target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::CDIO)
		endif()
	endif()
	if(NOT CDIO_FOUND)
		find_package(libcdio)
		if(TARGET libcdio::libcdio)
			target_compile_definitions(${PROJECT_NAME} PRIVATE USE_LIBCDIO)
			target_link_libraries(${PROJECT_NAME} PRIVATE libcdio::libcdio)
		endif()
	endif()
endif()

if(NOT VITA)
	target_compile_definitions(${PROJECT_NAME} PRIVATE USE_ICE)
	# libjuice
	option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
	option(NO_SERVER "Disable server support" ON)
	option(NO_TESTS "Disable tests build" ON)
	add_subdirectory(core/deps/libjuice EXCLUDE_FROM_ALL)
	target_link_libraries(${PROJECT_NAME} PRIVATE LibJuice::LibJuiceStatic)
endif()

# websocket++
target_include_directories(${PROJECT_NAME} PRIVATE core/deps/websocketpp)
target_compile_definitions(${PROJECT_NAME} PRIVATE ASIO_STANDALONE _WEBSOCKETPP_CPP11_STL_)

set(TINYGETTEXT_WO_ICONV ON)
add_subdirectory(core/deps/tinygettext EXCLUDE_FROM_ALL)
target_include_directories(${PROJECT_NAME} PRIVATE core/deps/tinygettext/include)
target_link_libraries(${PROJECT_NAME} PRIVATE tinygettext::tinygettext)

target_include_directories(${PROJECT_NAME} PRIVATE core/deps/picotcp/include core/deps/picotcp/modules)
target_sources(${PROJECT_NAME} PRIVATE
		core/deps/picotcp/include/arch/pico_arm9.h
		core/deps/picotcp/include/arch/pico_atsamd21j18.h
		core/deps/picotcp/include/arch/pico_avr.h
		core/deps/picotcp/include/arch/pico_cortex_m.h
		core/deps/picotcp/include/arch/pico_dos.h
		core/deps/picotcp/include/arch/pico_esp8266.h
		core/deps/picotcp/include/arch/pico_generic_gcc.h
		core/deps/picotcp/include/arch/pico_linux.h
		core/deps/picotcp/include/arch/pico_mbed.h
		core/deps/picotcp/include/arch/pico_msp430.h
		core/deps/picotcp/include/arch/pico_msvc.h
		core/deps/picotcp/include/arch/pico_none.h
		core/deps/picotcp/include/arch/pico_pic24.h
		core/deps/picotcp/include/arch/pico_pic32.h
		core/deps/picotcp/include/arch/pico_posix.h
		core/deps/picotcp/include/heap.h
		core/deps/picotcp/include/pico_addressing.h
		core/deps/picotcp/include/pico_config.h
		core/deps/picotcp/include/pico_constants.h
		core/deps/picotcp/include/pico_defines.h
		core/deps/picotcp/include/pico_defines_msvc.h
		core/deps/picotcp/include/pico_device.h
		core/deps/picotcp/include/pico_eth.h
		core/deps/picotcp/include/pico_frame.h
		core/deps/picotcp/include/pico_md5.h
		core/deps/picotcp/include/pico_module_eth.h
		core/deps/picotcp/include/pico_protocol.h
		core/deps/picotcp/include/pico_queue.h
		core/deps/picotcp/include/pico_socket.h
		core/deps/picotcp/include/pico_socket_multicast.h
		core/deps/picotcp/include/pico_stack.h
		core/deps/picotcp/include/pico_tree.h
		core/deps/picotcp/modules/pico_arp.c
		core/deps/picotcp/modules/pico_dev_ppp.c
		core/deps/picotcp/modules/pico_dhcp_common.c
		core/deps/picotcp/modules/pico_dhcp_common.h
		core/deps/picotcp/modules/pico_dhcp_server.c
		core/deps/picotcp/modules/pico_dhcp_server.h
		core/deps/picotcp/modules/pico_dns_client.c
		core/deps/picotcp/modules/pico_dns_common.c
		core/deps/picotcp/modules/pico_ethernet.c
		core/deps/picotcp/modules/pico_fragments.c
		core/deps/picotcp/modules/pico_icmp4.c
		core/deps/picotcp/modules/pico_ipv4.c
		core/deps/picotcp/modules/pico_socket_tcp.c
		core/deps/picotcp/modules/pico_socket_udp.c
		core/deps/picotcp/modules/pico_strings.c
		core/deps/picotcp/modules/pico_tcp.c
		core/deps/picotcp/modules/pico_udp.c
		core/deps/picotcp/stack/pico_device.c
		core/deps/picotcp/stack/pico_frame.c
		core/deps/picotcp/stack/pico_md5.c
		core/deps/picotcp/stack/pico_protocol.c
		core/deps/picotcp/stack/pico_socket.c
		core/deps/picotcp/stack/pico_socket_multicast.c
		core/deps/picotcp/stack/pico_stack.c
		core/deps/picotcp/stack/pico_tree.c)

target_compile_definitions(${PROJECT_NAME} PRIVATE _7ZIP_ST)
target_sources(${PROJECT_NAME} PRIVATE core/deps/lzma/7zArcIn.c core/deps/lzma/7zBuf.c core/deps/lzma/7zCrc.c core/deps/lzma/7zCrcOpt.c core/deps/lzma/7zDec.c core/deps/lzma/7zFile.c core/deps/lzma/7zStream.c core/deps/lzma/Alloc.c core/deps/lzma/Bcj2.c core/deps/lzma/Bra86.c core/deps/lzma/Bra.c core/deps/lzma/BraIA64.c core/deps/lzma/CpuArch.c core/deps/lzma/Delta.c core/deps/lzma/LzFind.c core/deps/lzma/Lzma2Dec.c core/deps/lzma/Lzma86Dec.c core/deps/lzma/Lzma86Enc.c core/deps/lzma/LzmaDec.c core/deps/lzma/LzmaEnc.c core/deps/lzma/LzmaLib.c core/deps/lzma/Sort.c)
add_subdirectory(core/deps/libelf)
target_link_libraries(${PROJECT_NAME} PRIVATE elf)
target_compile_definitions(${PROJECT_NAME} PRIVATE
	IMGUI_DISABLE_DEMO_WINDOWS
	IMGUI_DISABLE_DEBUG_TOOLS
	IMGUI_DEFINE_MATH_OPERATORS)
target_include_directories(${PROJECT_NAME} PRIVATE core/deps/imgui core/deps/imgui/backends)
target_sources(${PROJECT_NAME} PRIVATE
	core/deps/imgui/imgui.cpp
	core/deps/imgui/imgui_demo.cpp
	core/deps/imgui/imgui_draw.cpp
	core/deps/imgui/imgui_cjk.cpp
	core/deps/imgui/imgui_stdlib.cpp
	core/deps/imgui/imgui_tables.cpp
	core/deps/imgui/imgui_widgets.cpp
	core/deps/imgui/misc/freetype/imgui_freetype.cpp)

set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
add_subdirectory(core/deps/freetype EXCLUDE_FROM_ALL)
target_link_libraries(${PROJECT_NAME} PRIVATE freetype)

if(ENABLE_FC_PROFILER)
	target_include_directories(${PROJECT_NAME} PRIVATE core/deps/implot)
	target_sources(${PROJECT_NAME} PRIVATE
		core/deps/implot/implot.h
		core/deps/implot/implot.cpp
		core/deps/implot/implot_internal.h
		core/deps/implot/implot_items.cpp)
endif()
target_sources(${PROJECT_NAME} PRIVATE core/deps/xbrz/xbrz.cpp)
target_sources(${PROJECT_NAME} PRIVATE core/deps/md5/md5.cpp)

target_link_libraries(${PROJECT_NAME} PRIVATE flycast::res)

add_subdirectory(core/archive)
add_subdirectory(core/cfg)
add_subdirectory(core/hw)
add_subdirectory(core/imgread)
add_subdirectory(core/input)
add_subdirectory(core/log)
add_subdirectory(core/network)
add_subdirectory(core/oslib)
add_subdirectory(core/audio)
add_subdirectory(core/reios)
add_subdirectory(core/achievements)
add_subdirectory(core/ui)
add_subdirectory(core/lua)
add_subdirectory(core/profiler)
add_subdirectory(core/wsi)

include(resources/resources.cmake)

if(UNIX)
target_sources(${PROJECT_NAME} PRIVATE
		core/linux/common.cpp
		core/linux/context.cpp
		core/linux/posix_vmem.cpp
		core/linux/unwind_info.cpp
		core/linux-dist/evdev.cpp
		core/linux-dist/evdev.h
		core/linux-dist/icon.h
		core/linux-dist/x11.cpp
		core/linux-dist/x11.h
		core/linux-dist/x11_keyboard.h)
endif()

target_sources(${PROJECT_NAME} PRIVATE
		core/deps/rcheevos/src/rc_client_raintegration.c
		core/deps/rcheevos/src/rc_client.c
		core/deps/rcheevos/src/rc_compat.c
		core/deps/rcheevos/src/rc_util.c
		core/deps/rcheevos/src/rc_version.c
		core/deps/rcheevos/src/rapi/rc_api_common.c
		core/deps/rcheevos/src/rapi/rc_api_editor.c
		core/deps/rcheevos/src/rapi/rc_api_info.c
		core/deps/rcheevos/src/rapi/rc_api_runtime.c
		core/deps/rcheevos/src/rapi/rc_api_user.c
		core/deps/rcheevos/src/rcheevos/alloc.c
		core/deps/rcheevos/src/rcheevos/condition.c
		core/deps/rcheevos/src/rcheevos/condset.c
		core/deps/rcheevos/src/rcheevos/consoleinfo.c
		core/deps/rcheevos/src/rcheevos/format.c
		core/deps/rcheevos/src/rcheevos/lboard.c
		core/deps/rcheevos/src/rcheevos/memref.c
		core/deps/rcheevos/src/rcheevos/operand.c
		core/deps/rcheevos/src/rcheevos/rc_validate.c
		core/deps/rcheevos/src/rcheevos/richpresence.c
		core/deps/rcheevos/src/rcheevos/runtime_progress.c
		core/deps/rcheevos/src/rcheevos/runtime.c
		core/deps/rcheevos/src/rcheevos/trigger.c
		core/deps/rcheevos/src/rcheevos/value.c
		core/deps/rcheevos/src/rhash/aes.c
		core/deps/rcheevos/src/rhash/cdreader.c
		core/deps/rcheevos/src/rhash/hash.c
		core/deps/rcheevos/src/rhash/md5.c
		core/deps/rcheevos/src/rurl/url.c)
target_include_directories(${PROJECT_NAME} PRIVATE core/deps/rcheevos/include)
target_compile_definitions(${PROJECT_NAME} PRIVATE USE_RACHIEVEMENTS RC_DISABLE_LUA)

target_sources(${PROJECT_NAME} PRIVATE
		core/build.h
		core/chd.h
		core/cheats.cpp
		core/cheats.h
		core/emulator.h
		core/nullDC.cpp
		core/serialize.cpp
		core/serialize.h
		core/stbi.h
		core/stdclass.cpp
		core/stdclass.h
		core/types.h
		core/debug/gdb_server.h)

set(IMGUI_BACKENDS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/core/deps/imgui/backends")

add_subdirectory(core/rend)
if(NOT USE_OPENGL)
	# Headless: no renderer
	target_compile_definitions(${PROJECT_NAME} PRIVATE NO_REND)
endif()

if(ENABLE_GDB_SERVER)

	target_sources(${PROJECT_NAME} PRIVATE
		core/debug/gdb_server.cpp)

	target_compile_definitions(${PROJECT_NAME} PRIVATE GDB_SERVER)
endif()

if("arm" IN_LIST ARCHITECTURE)
	target_include_directories(${PROJECT_NAME} PRIVATE core/deps/vixl)
	target_sources(${PROJECT_NAME} PRIVATE
		core/rec-ARM/rec_arm.cpp
		core/deps/vixl/aarch32/location-aarch32.cc
		core/deps/vixl/aarch32/location-aarch32.h
		core/deps/vixl/aarch32/assembler-aarch32.cc
		core/deps/vixl/aarch32/assembler-aarch32.h
		core/deps/vixl/aarch32/instructions-aarch32.cc
		core/deps/vixl/aarch32/instructions-aarch32.h
		core/deps/vixl/aarch32/constants-aarch32.cc
		core/deps/vixl/aarch32/constants-aarch32.h
		core/deps/vixl/aarch32/macro-assembler-aarch32.cc
		core/deps/vixl/aarch32/macro-assembler-aarch32.h
		core/deps/vixl/aarch32/operands-aarch32.cc
		core/deps/vixl/aarch32/operands-aarch32.h
		core/deps/vixl/assembler-base-vixl.h
		core/deps/vixl/code-buffer-vixl.cc
		core/deps/vixl/code-buffer-vixl.h
		core/deps/vixl/code-generation-scopes-vixl.h
		core/deps/vixl/compiler-intrinsics-vixl.cc
		core/deps/vixl/compiler-intrinsics-vixl.h
		core/deps/vixl/cpu-features.cc
		core/deps/vixl/cpu-features.h
		core/deps/vixl/globals-vixl.h
		core/deps/vixl/invalset-vixl.h
		core/deps/vixl/macro-assembler-interface.h
		core/deps/vixl/platform-vixl.h
		core/deps/vixl/pool-manager.h
		core/deps/vixl/pool-manager-impl.h
		core/deps/vixl/utils-vixl.cc
		core/deps/vixl/utils-vixl.h)
else()
	# The only dynarecs are for 32-bit ARM. Other hosts use the interpreters.
	target_compile_definitions(${PROJECT_NAME} PRIVATE TARGET_NO_REC)
endif()

if(USE_OPENGL OR USE_GLES2 OR USE_GLES)
	add_library(glad STATIC core/deps/glad/src/gl.c)
	if(NOT SDL2_FOUND)
		# When SDL2 is not found, we can use EGL with X11
		target_sources(glad PRIVATE core/deps/glad/src/egl.c)
	endif()
	target_include_directories(glad PUBLIC core/deps/glad/include)
	target_link_libraries(${PROJECT_NAME} PRIVATE glad)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE core/deps/ggpo/include core/deps/ggpo/lib/ggpo)
target_sources(${PROJECT_NAME} PRIVATE
	core/deps/ggpo/lib/ggpo/bitvector.cpp
	core/deps/ggpo/lib/ggpo/bitvector.h
	core/deps/ggpo/lib/ggpo/game_input.cpp
	core/deps/ggpo/lib/ggpo/game_input.h
	core/deps/ggpo/lib/ggpo/input_queue.cpp
	core/deps/ggpo/lib/ggpo/input_queue.h
	core/deps/ggpo/lib/ggpo/main.cpp
	core/deps/ggpo/lib/ggpo/platform_linux.cpp
	core/deps/ggpo/lib/ggpo/platform_linux.h
	core/deps/ggpo/lib/ggpo/poll.cpp
	core/deps/ggpo/lib/ggpo/ggpo_poll.h
	core/deps/ggpo/lib/ggpo/ring_buffer.h
	core/deps/ggpo/lib/ggpo/static_buffer.h
	core/deps/ggpo/lib/ggpo/sync.cpp
	core/deps/ggpo/lib/ggpo/sync.h
	core/deps/ggpo/lib/ggpo/timesync.cpp
	core/deps/ggpo/lib/ggpo/timesync.h
	core/deps/ggpo/lib/ggpo/ggpo_types.h

	core/deps/ggpo/lib/ggpo/backends/backend.h
	core/deps/ggpo/lib/ggpo/backends/p2p.cpp
	core/deps/ggpo/lib/ggpo/backends/p2p.h
	core/deps/ggpo/lib/ggpo/backends/spectator.cpp
	core/deps/ggpo/lib/ggpo/backends/spectator.h
	core/deps/ggpo/lib/ggpo/backends/synctest.cpp
	core/deps/ggpo/lib/ggpo/backends/synctest.h

	core/deps/ggpo/lib/ggpo/network/udp_msg.h
	core/deps/ggpo/lib/ggpo/network/udp_proto.cpp
	core/deps/ggpo/lib/ggpo/network/udp_proto.h
	core/deps/ggpo/lib/ggpo/network/udp.cpp
	core/deps/ggpo/lib/ggpo/network/udp.h)

if(NOT BUILD_TESTING AND NOT VITA)
	target_sources(${PROJECT_NAME} PRIVATE core/linux-dist/main.cpp)
endif()

if(BUILD_TESTING)
	add_subdirectory(core/deps/googletest EXCLUDE_FROM_ALL)
	add_subdirectory(tests)
endif()

if(VITA)
	vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE NOASLR)
	vita_create_vpk(Flycast.vpk FLYCASTDC eboot.bin
		NAME "Flycast"
		VERSION 01.00
		FILE ${CMAKE_SOURCE_DIR}/shell/vita/icon0.png sce_sys/icon0.png
		${CMAKE_SOURCE_DIR}/shell/vita/bg.png sce_sys/livearea/contents/bg.png
		${CMAKE_SOURCE_DIR}/shell/vita/startup.png sce_sys/livearea/contents/startup.png
		${CMAKE_SOURCE_DIR}/shell/vita/template.xml sce_sys/livearea/contents/template.xml)
else()
	install(TARGETS ${PROJECT_NAME} DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
