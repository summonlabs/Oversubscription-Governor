cmake_minimum_required(VERSION 3.20)
project(oversub_consumer LANGUAGES CXX)

# Independent downstream consumer: it knows nothing about this repository except
# the installed package. It fails if the package does not export its target,
# headers, dependency on Threads, and version file.
find_package(OversubscriptionGovernor 1.0 CONFIG REQUIRED)

add_executable(oversub_consumer main.cpp)
target_link_libraries(oversub_consumer PRIVATE OversubscriptionGovernor::OversubscriptionGovernor)
