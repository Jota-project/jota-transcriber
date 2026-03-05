cmake_minimum_required(VERSION 3.16)
project(transcription_service)

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

    # Mosquitto C++ (MQTT)
    find_path(MOSQUITTO_INCLUDE_DIR mosquittopp.h)
    find_library(MOSQUITTO_LIBRARY mosquitto)
    find_library(MOSQUITTO_PP_LIBRARY mosquittopp)

    if(MOSQUITTO_INCLUDE_DIR AND MOSQUITTO_LIBRARY AND MOSQUITTO_PP_LIBRARY)
        set(MOSQUITTO_FOUND TRUE)
        set(MOSQUITTO_LIBRARIES ${MOSQUITTO_PP_LIBRARY} ${MOSQUITTO_LIBRARY})
        set(MOSQUITTO_INCLUDE_DIRS ${MOSQUITTO_INCLUDE_DIR})
        message(STATUS "Mosquitto C++ found: ${MOSQUITTO_INCLUDE_DIR}")
    else()
        message(FATAL_ERROR "Mosquitto C++ not found. Install libmosquittopp-dev.")
    endif()

    add_executable(transcription_server
        src/server.cpp
        src/server/StreamingSession.cpp
        src/server/AuthManager.cpp
        src/server/ConnectionLimiter.cpp
        src/server/ConnectionGuard.cpp
        src/mqtt/MQTTPublisher.cpp
        src/auth/ApiAuthClient.cpp
        src/auth/AuthCache.cpp
    )

    target_include_directories(transcription_server PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${MOSQUITTO_INCLUDE_DIRS}
    )

    target_link_libraries(transcription_server PRIVATE
        Boost::boost
        pthread
        streaming_whisper
        nlohmann_json::nlohmann_json
        OpenSSL::SSL
        OpenSSL::Crypto
        ${MOSQUITTO_LIBRARIES}
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