add_library(OversubscriptionGovernorDist STATIC
  protocol.cpp
  coordinator.cpp
  worker.cpp)
target_include_directories(OversubscriptionGovernorDist PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(OversubscriptionGovernorDist PUBLIC OversubscriptionGovernor::OversubscriptionGovernor)
set_target_properties(OversubscriptionGovernorDist PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(oversub_dist main.cpp)
target_link_libraries(oversub_dist PRIVATE OversubscriptionGovernorDist)
