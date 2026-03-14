cmake_minimum_required(VERSION 3.16)

add_executable(unit_tests
    unit/test_hallucination_guard.cpp
    unit/test_audio_pipeline.cpp
    unit/test_inference_limiter.cpp
    unit/test_connection_limiter.cpp
    unit/test_session_tracker.cpp
    unit/test_model_cache.cpp
    unit/test_streaming_whisper_engine.cpp
    unit/test_streaming_session.cpp
    # Fuentes del servidor que no tienen dependencias de Boost/OpenSSL
    ${CMAKE_SOURCE_DIR}/src/server/ConnectionLimiter.cpp
    ${CMAKE_SOURCE_DIR}/src/server/ConnectionGuard.cpp
    ${CMAKE_SOURCE_DIR}/src/server/AuthManager.cpp
    ${CMAKE_SOURCE_DIR}/src/auth/AuthCache.cpp
    ${CMAKE_SOURCE_DIR}/src/auth/ApiAuthClient.cpp
)

find_package(Boost REQUIRED)
find_package(OpenSSL REQUIRED)

# nlohmann_json: fetched by BUILD_SERVER when ON; fetch here when OFF
if(NOT TARGET nlohmann_json::nlohmann_json)
    include(FetchContent)
    FetchContent_Declare(json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(json)
endif()

target_link_libraries(unit_tests
    streaming_whisper
    GTest::gtest_main
    Boost::boost
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)

target_include_directories(unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(unit_tests PRIVATE
    PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
)

include(GoogleTest)
gtest_discover_tests(unit_tests)
