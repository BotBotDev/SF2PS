cmake_minimum_required(VERSION 3.13)
project(sf2server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
add_compile_options(
    -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion
    -fno-plt -fvisibility=hidden
    -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE
)
add_link_options(-Wl,-z,relro,-z,now -Wl,-z,noexecstack -pie)

option(SF2_SANITIZE "build with ASan/UBSan" OFF)
if(SF2_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

find_path(SQLITE3_INCLUDE_DIR sqlite3.h)
find_library(SQLITE3_LIBRARY NAMES sqlite3)
if(NOT SQLITE3_INCLUDE_DIR OR NOT SQLITE3_LIBRARY)
    message(FATAL_ERROR "sqlite3 development files not found (apt install libsqlite3-dev)")
endif()

set(SF2_CORE_SOURCES
    src/sfs.cpp
    src/protobuf.cpp
    src/zlibx.cpp
    src/packet.cpp
    src/json.cpp
    src/crypto.cpp
    src/validate.cpp
    src/log.cpp
    src/config.cpp
    src/player_proto.cpp
    src/protocol_data.cpp
    src/responses.cpp
    src/xml.cpp
    src/progress_data.cpp
    src/gamedata.cpp
    src/raid.cpp
    src/raid_report.cpp
    src/schedule.cpp
    src/social_proto.cpp
)

add_library(sf2core STATIC ${SF2_CORE_SOURCES})
target_include_directories(sf2core PUBLIC include)
target_link_libraries(sf2core PUBLIC ZLIB::ZLIB Threads::Threads)

add_library(sf2net STATIC
    src/db.cpp
    src/db_progress.cpp
    src/db_social.cpp
    src/server.cpp
    src/handlers_core.cpp
    src/handlers_raid.cpp
)
target_include_directories(sf2net PUBLIC include ${SQLITE3_INCLUDE_DIR})
target_link_libraries(sf2net PUBLIC sf2core ${SQLITE3_LIBRARY})

add_executable(sf2server src/main.cpp)
target_link_libraries(sf2server PRIVATE sf2net)

install(TARGETS sf2server RUNTIME DESTINATION bin)
install(DIRECTORY config/ DESTINATION share/sf2server/config)
