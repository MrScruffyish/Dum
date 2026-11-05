cmake_minimum_required(VERSION 3.16)
project(Dum CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# --- ENet (vendored in third_party/enet, MIT license) -----------------------
file(GLOB ENET_SOURCES third_party/enet/*.c)
add_library(enet STATIC ${ENET_SOURCES})
target_include_directories(enet PUBLIC third_party/enet/include)
if(WIN32)
  target_link_libraries(enet PUBLIC ws2_32 winmm)
else()
  target_compile_definitions(enet PRIVATE HAS_FCNTL=1 HAS_POLL=1 HAS_GETADDRINFO=1 HAS_GETNAMEINFO=1
    HAS_GETHOSTBYNAME_R=1 HAS_GETHOSTBYADDR_R=1 HAS_INET_PTON=1 HAS_INET_NTOP=1 HAS_MSGHDR_FLAGS=1 HAS_SOCKLEN_T=1)
endif()

# --- SDL2 ---------------------------------------------------------------------
# Windows/macOS/vcpkg: SDL's own CMake config files. Linux: pkg-config.
if(NOT WIN32)
  find_package(PkgConfig QUIET)
endif()
if(PKG_CONFIG_FOUND)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2 SDL2_image)
  set(SDL_LIBS PkgConfig::SDL2)
else()
  find_package(SDL2 REQUIRED CONFIG)
  find_package(SDL2_image REQUIRED CONFIG)
  set(SDL_LIBS SDL2::SDL2main SDL2::SDL2 SDL2_image::SDL2_image)
endif()

# --- Dum ----------------------------------------------------------------------
add_executable(dum
  src/main.cpp
  src/game/data.cpp
  src/game/map.cpp
  src/game/campaign.cpp
  src/game/state.cpp
  src/net/net.cpp
  src/server/server.cpp
  src/server/save.cpp
  src/client/client.cpp
  src/client/autopilot.cpp
  src/client/renderer.cpp
  src/client/gpu.cpp
  src/client/textures.cpp
  src/client/sound.cpp
)
target_include_directories(dum PRIVATE src)
target_link_libraries(dum PRIVATE enet ${SDL_LIBS})
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(dum PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

find_package(Threads REQUIRED)
target_link_libraries(dum PRIVATE Threads::Threads)
if(WIN32)
  # One self-contained exe (no libstdc++/winpthread DLLs), no console window in release.
  target_link_options(dum PRIVATE -static -static-libgcc -static-libstdc++)
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(dum PRIVATE -mwindows)
  endif()
endif()

# The game looks for data/ next to the executable, then in the parent directory,
# so build/dum uses the repository's data/ directly (edit and restart, no rebuild).

# --- headless self test (no SDL needed) --------------------------------------
add_executable(dum_selftest
  tests/selftest.cpp
  src/game/data.cpp
  src/game/map.cpp
  src/game/campaign.cpp
  src/game/state.cpp
  src/net/net.cpp
  src/server/server.cpp
  src/server/save.cpp
)
target_include_directories(dum_selftest PRIVATE src)
target_link_libraries(dum_selftest PRIVATE enet)
add_dependencies(dum_selftest dum)

# --- content tool: maps, campaigns, checks (no SDL needed) --------------------
add_executable(dum_tool
  src/tools/dum_tool.cpp
  src/game/data.cpp
  src/game/map.cpp
  src/game/campaign.cpp
)
target_include_directories(dum_tool PRIVATE src)

if(WIN32)
  # Self-contained tools as well (no MinGW runtime DLLs needed).
  target_link_options(dum_tool PRIVATE -static -static-libgcc -static-libstdc++)
  target_link_options(dum_selftest PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# cmake --install build --prefix <dir>: the game, the content tool, data, docs and licences.
install(TARGETS dum dum_tool DESTINATION .)
install(DIRECTORY data docs DESTINATION .)
install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md DESTINATION .)
