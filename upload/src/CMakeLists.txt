cmake_minimum_required(VERSION 3.10)
project(timestamp_project)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find ZeroMQ package
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZeroMQ REQUIRED libzmq)

# Add include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${ZeroMQ_INCLUDE_DIRS}
)

# Add source files
set(SOURCES
    main.cpp
    common.cpp
    streams.cpp
)

# Create executable
add_executable(timestamp_client ${SOURCES})

# Link libraries
target_link_libraries(timestamp_client
    ${ZeroMQ_LIBRARIES}
    pthread
)

# Install target
install(TARGETS timestamp_client DESTINATION bin)

# Copy configuration files
install(FILES 
    config/DataLinkTargetService.conf
    config/DataLinkTargetService.log.conf
    DESTINATION etc/elvis)
