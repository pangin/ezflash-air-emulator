cmake_minimum_required(VERSION 3.10)

# mGBA 0.10.5's CMake uses pre-3.5 policies; allow modern CMake to configure it.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

project(ezflash_air_emulator C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# --- Build only the minimal libmgba GBA core (no frontends, no optional deps) ---
set(BUILD_QT      OFF CACHE BOOL "" FORCE)
set(BUILD_SDL     OFF CACHE BOOL "" FORCE)
set(BUILD_GL      OFF CACHE BOOL "" FORCE)
set(BUILD_GLES2   OFF CACHE BOOL "" FORCE)
set(BUILD_GLES3   OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED  OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC  ON  CACHE BOOL "" FORCE)
set(USE_FFMPEG    OFF CACHE BOOL "" FORCE)
set(USE_LUA       OFF CACHE BOOL "" FORCE)
set(ENABLE_SCRIPTING OFF CACHE BOOL "" FORCE)
set(USE_DISCORD_RPC  OFF CACHE BOOL "" FORCE)
set(USE_LIBZIP    OFF CACHE BOOL "" FORCE)
set(USE_SQLITE3   OFF CACHE BOOL "" FORCE)
set(USE_MINIZIP   OFF CACHE BOOL "" FORCE)
set(USE_LZMA      OFF CACHE BOOL "" FORCE)
set(USE_EDITLINE  OFF CACHE BOOL "" FORCE)
set(USE_DEBUGGERS OFF CACHE BOOL "" FORCE)
set(USE_GDB_STUB  OFF CACHE BOOL "" FORCE)
set(USE_ELF       OFF CACHE BOOL "" FORCE)
set(USE_EPOXY     OFF CACHE BOOL "" FORCE)
set(M_CORE_GB     OFF CACHE BOOL "" FORCE)
set(M_CORE_GBA    ON  CACHE BOOL "" FORCE)

add_subdirectory(third_party/mgba EXCLUDE_FROM_ALL)

# --- EZ-FLASH Air cartridge model: pure, core-agnostic static library ---
# Built with NO mGBA include path, so the one-way dependency (adapter -> cart)
# is enforced by the build itself: ez_cart.c cannot accidentally use mGBA.
add_library(ez_cart STATIC src/ez_cart.c)
target_include_directories(ez_cart PUBLIC ${CMAKE_SOURCE_DIR}/include)

# The static core target built by mGBA is named "mgba" (libmgba.a).
# ez_mgba_adapter.c is the only TU that bridges mGBA <-> ez_cart.
add_executable(ezair-runner
  src/ezair_runner.c
  src/ez_mgba_adapter.c
)
target_include_directories(ezair-runner PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/third_party/mgba/include
  ${CMAKE_BINARY_DIR}/third_party/mgba/include
)
# IMPORTANT: match libmgba's color ABI. This build does NOT define COLOR_16_BIT
# (only BUILD_BBB/RASPI/PANDORA would), so color_t == uint32_t. Do not define it.
target_link_libraries(ezair-runner PRIVATE ez_cart mgba)

# --- cart-core unit tests (mGBA-free) ---
# Links ONLY ez_cart, so it builds and runs in seconds without the GBA core.
include(CTest)
if(BUILD_TESTING)
  add_executable(ez_cart_test tests/ez_cart_test.c)
  target_link_libraries(ez_cart_test PRIVATE ez_cart)
  add_test(NAME ez_cart_test COMMAND ez_cart_test)
endif()
