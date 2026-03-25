cmake_minimum_required(VERSION 3.20)
project(nanoclaw VERSION 1.2.25 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find packages via vcpkg
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(SQLiteCpp CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS filesystem)
find_package(httplib CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(croncpp CONFIG REQUIRED)
find_package(TgBot CONFIG REQUIRED)
find_package(Threads REQUIRED)

# Collect all source files
file(GLOB_RECURSE SOURCES "src/*.cpp")

add_executable(nanoclaw ${SOURCES})

target_include_directories(nanoclaw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(nanoclaw PRIVATE
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    SQLiteCpp
    Boost::filesystem
    httplib::httplib
    yaml-cpp::yaml-cpp
    croncpp::croncpp
    TgBot::TgBot
    Threads::Threads
)

if(WIN32)
    target_link_libraries(nanoclaw PRIVATE ws2_32 shell32)
    target_compile_definitions(nanoclaw PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# Install target
install(TARGETS nanoclaw RUNTIME DESTINATION bin)
