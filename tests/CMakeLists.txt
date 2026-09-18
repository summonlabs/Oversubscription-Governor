set(OVERSUB_TESTS
  core_tests
  property_tests
  adversarial_tests
  concurrency_tests
  persistence_tests
  net_tests
)

find_package(Threads REQUIRED)

foreach(t ${OVERSUB_TESTS})
  add_executable(${t} src/main.cpp src/${t}.cpp)
  target_link_libraries(${t} PRIVATE OversubscriptionGovernor::OversubscriptionGovernor Threads::Threads)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

if(OVERSUB_BUILD_DIST)
  add_executable(distributed_tests src/main.cpp src/distributed_tests.cpp)
  target_link_libraries(distributed_tests PRIVATE OversubscriptionGovernorDist Threads::Threads)
  target_include_directories(distributed_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/dist)
  target_compile_definitions(distributed_tests PRIVATE OVERSUB_DIST_BINARY="$<TARGET_FILE:oversub_dist>")
  add_dependencies(distributed_tests oversub_dist)
  add_test(NAME distributed_tests COMMAND distributed_tests)
endif()
