cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

project(pixiv_scrapper LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Handle GCC 14+ strict C pointer types for older C dependencies (libzip, etc.)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-error=incompatible-pointer-types -Wno-incompatible-pointer-types")
endif()

# For MSVC: static runtime (/MT instead of /MD) so vcruntime140.dll is not needed
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# On Windows static builds: prefer .a/.lib
if(WIN32)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries" FORCE)
    if(MINGW)
        set(CMAKE_FIND_LIBRARY_SUFFIXES ".a" ".lib")
    endif()
endif()

include(FetchContent)

# ---------------------------------------------------------------------------
# 1. Threads
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# 2. SQLite3
# ---------------------------------------------------------------------------
find_package(unofficial-sqlite3 CONFIG QUIET)
if(unofficial-sqlite3_FOUND)
    set(SQLITE3_TARGET unofficial::sqlite3::sqlite3)
else()
    find_package(SQLite3 QUIET)
    if(SQLite3_FOUND)
        set(SQLITE3_TARGET SQLite::SQLite3)
    else()
        message(STATUS "SQLite3 not found on system; building from source via FetchContent...")
        FetchContent_Declare(
            sqlite3_src
            URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
        )
        FetchContent_MakeAvailable(sqlite3_src)
        if(NOT TARGET SQLite::SQLite3)
            add_library(sqlite3 STATIC "${sqlite3_src_SOURCE_DIR}/sqlite3.c")
            target_include_directories(sqlite3 PUBLIC "${sqlite3_src_SOURCE_DIR}")
            add_library(SQLite::SQLite3 ALIAS sqlite3)
        endif()
        set(SQLITE3_TARGET SQLite::SQLite3)
    endif()
endif()

# ---------------------------------------------------------------------------
# 3. ZLIB & libzip
# ---------------------------------------------------------------------------
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    if(WIN32 AND MINGW)
        list(APPEND PKG_CONFIG_EXECUTABLE "--static")
    endif()
    pkg_check_modules(LIBZIP QUIET IMPORTED_TARGET libzip)
    if(TARGET PkgConfig::LIBZIP)
        set(LIBZIP_TARGET PkgConfig::LIBZIP)
    endif()
endif()

if(NOT LIBZIP_TARGET)
    find_package(libzip CONFIG QUIET)
    if(libzip_FOUND)
        set(LIBZIP_TARGET libzip::zip)
    endif()
endif()

if(NOT LIBZIP_TARGET)
    message(STATUS "libzip not found on system; building zlib & libzip via FetchContent...")

    # Check if ZLIB is already available or fetch it
    if(NOT ZLIB_FETCHED)
        find_package(ZLIB QUIET)
    endif()

    if(NOT ZLIB_FOUND OR ZLIB_FETCHED)
        set(ZLIB_FETCHED TRUE CACHE BOOL "ZLIB is provided via FetchContent" FORCE)
        set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            zlib
            URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
        )
        FetchContent_MakeAvailable(zlib)
        if(TARGET zlibstatic)
            set(ZLIB_LIB_TARGET zlibstatic)
        else()
            set(ZLIB_LIB_TARGET zlib)
        endif()
        set(ZLIB_LIBRARY ${ZLIB_LIB_TARGET} CACHE STRING "" FORCE)
        set(ZLIB_LIBRARIES ${ZLIB_LIB_TARGET} CACHE STRING "" FORCE)
        set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}" CACHE STRING "" FORCE)
        set(ZLIB_INCLUDE_DIRS "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}" CACHE STRING "" FORCE)
        set(ZLIB_FOUND TRUE CACHE BOOL "" FORCE)
        if(NOT TARGET ZLIB::ZLIB)
            add_library(ZLIB::ZLIB ALIAS ${ZLIB_LIB_TARGET})
        endif()
    endif()

    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(BUILD_REGRESS OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_DOC OFF CACHE BOOL "" FORCE)
    set(ENABLE_BZIP2 OFF CACHE BOOL "" FORCE)
    set(ENABLE_LZMA OFF CACHE BOOL "" FORCE)
    set(ENABLE_ZSTD OFF CACHE BOOL "" FORCE)
    set(LIBZIP_DO_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        libzip
        URL https://github.com/nih-at/libzip/releases/download/v1.10.1/libzip-1.10.1.tar.gz
    )
    FetchContent_MakeAvailable(libzip)
    set(LIBZIP_TARGET libzip::zip)
endif()

# ---------------------------------------------------------------------------
# 4. CURL
# ---------------------------------------------------------------------------
find_package(CURL CONFIG QUIET)
if(NOT CURL_FOUND)
    find_package(CURL QUIET)
endif()

if(CURL_FOUND)
    set(CURL_TARGET CURL::libcurl)
else()
    message(STATUS "CURL not found on system; building via FetchContent...")
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(HTTP_ONLY ON CACHE BOOL "" FORCE)
    set(CURL_ENABLE_EXPORT_TARGET OFF CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    if(WIN32)
        set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(
        curl
        URL https://github.com/curl/curl/releases/download/curl-8_10_1/curl-8.10.1.tar.gz
    )
    FetchContent_MakeAvailable(curl)
    set(CURL_TARGET CURL::libcurl)
endif()

# ---------------------------------------------------------------------------
# 5. CLI11 & nlohmann_json
# ---------------------------------------------------------------------------
find_package(CLI11 CONFIG QUIET)
if(NOT CLI11_FOUND)
    FetchContent_Declare(
        cli11
        URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.4.2.tar.gz
    )
    FetchContent_MakeAvailable(cli11)
endif()

find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ---------------------------------------------------------------------------
# 6. Third-party single header files (stb_image, gif-h)
# ---------------------------------------------------------------------------
set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
file(MAKE_DIRECTORY "${THIRD_PARTY_DIR}")

if(NOT EXISTS "${THIRD_PARTY_DIR}/stb_image.h")
    message(STATUS "Downloading stb_image.h...")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h"
        "${THIRD_PARTY_DIR}/stb_image.h"
        TLS_VERIFY ON
    )
endif()

if(NOT EXISTS "${THIRD_PARTY_DIR}/gif.h")
    message(STATUS "Downloading gif.h...")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/charlietangora/gif-h/master/gif.h"
        "${THIRD_PARTY_DIR}/gif.h"
        TLS_VERIFY ON
    )
endif()

# ---------------------------------------------------------------------------
# 7. Executable target
# ---------------------------------------------------------------------------
add_executable(pixiv_scrapper main.cpp)

target_include_directories(pixiv_scrapper PRIVATE "${THIRD_PARTY_DIR}")

target_link_libraries(pixiv_scrapper PRIVATE
    Threads::Threads
    ${CURL_TARGET}
    ${SQLITE3_TARGET}
    ${LIBZIP_TARGET}
    CLI11::CLI11
    nlohmann_json::nlohmann_json
)

# ---------------------------------------------------------------------------
# 8. Platform specific linker options
# ---------------------------------------------------------------------------
if(WIN32)
    target_compile_definitions(pixiv_scrapper PRIVATE
        CURL_STATICLIB
        ZIP_STATIC
    )
    # Required Windows system libraries when statically linking libcurl and sockets
    target_link_libraries(pixiv_scrapper PRIVATE
        ws2_32
        crypt32
        wldap32
        normaliz
        bcrypt
        propsys
        ole32
        shell32
    )
    if(MINGW)
        target_link_options(pixiv_scrapper PRIVATE
            -static
            -static-libgcc
            -static-libstdc++
        )
    endif()
elseif(UNIX AND NOT APPLE)
    # On Linux, dl is often needed by sqlite / threads
    target_link_libraries(pixiv_scrapper PRIVATE ${CMAKE_DL_LIBS})
endif()
