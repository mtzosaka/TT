cmake_minimum_required(VERSION 3.10)
project(distributed_timestamp)

# Find ZeroMQ package
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED libzmq)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include directories
include_directories(${CMAKE_SOURCE_DIR} ${ZMQ_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/include)

# Copy header files to build directory
configure_file(${CMAKE_SOURCE_DIR}/fixed_updated_master_controller.hpp
               ${CMAKE_BINARY_DIR}/fixed_master_controller.hpp COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/fixed_updated_slave_agent.hpp
               ${CMAKE_BINARY_DIR}/fixed_slave_agent.hpp COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/streams.hpp
               ${CMAKE_BINARY_DIR}/streams.hpp COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/common.hpp
               ${CMAKE_BINARY_DIR}/common.hpp COPYONLY)

# Master executable
add_executable(master_timestamp
    master_main.cpp
    fixed_enhanced_master_controller.cpp
    streams.cpp
    working_common.cpp
)

# Slave executable
add_executable(slave_timestamp
    slave_main.cpp
    fixed_enhanced_slave_agent.cpp
    streams.cpp
    working_common.cpp
)

# Link libraries
target_link_libraries(master_timestamp ${ZMQ_LIBRARIES})
target_link_libraries(slave_timestamp ${ZMQ_LIBRARIES})

# Add filesystem library for GCC < 9.0
if(CMAKE_COMPILER_IS_GNUCXX AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(master_timestamp stdc++fs)
    target_link_libraries(slave_timestamp stdc++fs)
endif()
