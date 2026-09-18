add_executable(oversub_cli main.cpp)
target_link_libraries(oversub_cli PRIVATE OversubscriptionGovernor::OversubscriptionGovernor)
install(TARGETS oversub_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
