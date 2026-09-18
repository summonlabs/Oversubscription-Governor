set(OVERSUB_EXAMPLES
  within_policy
  reduction_required
  capacity_collapse
  stale_evidence
  policy_change
  restart_recovery
  epoch_advance
)

foreach(example ${OVERSUB_EXAMPLES})
  add_executable(${example} ${example}.cpp)
  target_link_libraries(${example} PRIVATE OversubscriptionGovernor::OversubscriptionGovernor)
  target_include_directories(${example} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()
