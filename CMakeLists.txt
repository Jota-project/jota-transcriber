cmake_minimum_required(VERSION 3.16)
project(jota-transcriber)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Opciones
option(BUILD_TESTS "Build tests" ON)
option(BUILD_SERVER "Build the server executable" ON)

# Agregar whisper.cpp como subdirectorio
add_subdirectory(third_party/whisper.cpp)

# Librería StreamingWhisperEngine (independiente de Boost/OpenSSL)
add_library(streaming_whisper
    src/whisper/StreamingWhisperEngine.cpp
)

target_include_directories(streaming_whisper PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/whisper.cpp/include
)

target_link_libraries(streaming_whisper PUBLIC
    whisper
)

# Servidor principal (requiere Boost, OpenSSL, nlohmann_json)
if(BUILD_SERVER)
    find_package(Boost REQUIRED)
    find_package(OpenSSL REQUIRED)

    # nlohmann/json para protocolo JSON
    include(FetchContent)
    FetchContent_Declare(json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(json)

    add_executable(jota-transcriber
        src/server.cpp
        src/server/StreamingSession.cpp
        src/server/AuthManager.cpp
        src/server/ConnectionLimiter.cpp
        src/server/ConnectionGuard.cpp
        src/auth/ApiAuthClient.cpp
        src/auth/AuthCache.cpp
    )

    target_include_directories(jota-transcriber PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include
        ${Boost_INCLUDE_DIRS}
    )

    target_link_libraries(jota-transcriber PRIVATE
        whisper
        Boost::boost
        pthread
        streaming_whisper
        nlohmann_json::nlohmann_json
        OpenSSL::SSL
        OpenSSL::Crypto
    )
endif()

# Tests
if(BUILD_TESTS)
    enable_testing()
    # Descargar Google Test
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    # Para Windows: Prevenir sobrescribir las flags del proyecto padre
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    
    add_subdirectory(tests)
endif()