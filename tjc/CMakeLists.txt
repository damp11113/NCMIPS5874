cmake_minimum_required(VERSION 3.10)
project(tjc CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)
add_executable(tjc tjc.cpp)
add_executable(tjc_test tjc_test.cpp)
target_link_libraries(tjc PRIVATE Threads::Threads)
target_link_libraries(tjc_test PRIVATE Threads::Threads)

enable_testing()
add_test(NAME tjc_test COMMAND tjc_test)
