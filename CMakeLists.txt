cmake_minimum_required(VERSION 3.16)
project(launcher-kernel VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC OFF)

# Usage: cmake -S . -B build -DCMAKE_PREFIX_PATH="E:/Qt/6.11.1/mingw_64"
find_package(Qt6 REQUIRED COMPONENTS Core Network)

# ===== libmcbase (static library) =====
set(LIBMCBASE_SRC
    libmcbase/src/mc_i18n.cpp
    libmcbase/src/mc_log.cpp
    libmcbase/src/mc_str.cpp
    libmcbase/src/mc_path.cpp
    libmcbase/src/mc_hash.cpp
    libmcbase/src/mc_http.cpp
    libmcbase/src/mc_zip.cpp
    libmcbase/src/mc_download.cpp
    libmcbase/src/mc_download_qt.cpp
    libmcbase/src/mc_version.cpp
    libmcbase/src/mc_library.cpp
    libmcbase/src/mc_manifest.cpp
    libmcbase/src/mc_mod.cpp
)

add_library(libmcbase STATIC ${LIBMCBASE_SRC})
target_include_directories(libmcbase PUBLIC libmcbase/include)
target_link_libraries(libmcbase PUBLIC Qt6::Core Qt6::Network z)

# ===== Programs =====
function(add_mc_program NAME)
    add_executable(${NAME} ${NAME}/main.cpp ${ARGN})
    target_link_libraries(${NAME} PRIVATE libmcbase)
endfunction()

add_mc_program(mcver)
add_mc_program(mcsearch)
add_mc_program(modsearch)
add_mc_program(modver)
add_mc_program(packsearch)
add_mc_program(installer)

add_mc_program(mcjava
    mcjava/private/mc_java.cpp
)
target_include_directories(mcjava PRIVATE mcjava/private/include)

add_mc_program(login
    login/private/mc_auth.cpp
    login/private/mc_auth_msa.cpp
)
target_include_directories(login PRIVATE login/private/include)

add_mc_program(downloader
    downloader/private/mc_asset.cpp
    downloader/private/mc_java_dl.cpp
)
target_include_directories(downloader PRIVATE downloader/private/include)

add_mc_program(mclaunch
    login/private/mc_auth.cpp
    login/private/mc_auth_msa.cpp
)
target_include_directories(mclaunch PRIVATE login/private/include)

# ===== Post-build: copy artifacts to dist/ =====
set(MC_PROGRAMS mcver mcsearch modsearch modver packsearch mcjava login downloader installer mclaunch)

foreach(PROG ${MC_PROGRAMS})
    add_custom_command(TARGET ${PROG} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/dist"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${PROG}>" "${CMAKE_SOURCE_DIR}/dist/"
        COMMENT "Copying ${PROG} to dist/"
    )
endforeach()

add_custom_target(copy-lang ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/lang" "${CMAKE_SOURCE_DIR}/dist/lang"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/mirrors.json" "${CMAKE_SOURCE_DIR}/dist/"
    COMMENT "Copying lang/ and mirrors.json to dist/"
)
add_dependencies(copy-lang ${MC_PROGRAMS})
