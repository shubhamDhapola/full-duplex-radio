add_executable(impair
  main.cpp
  scenario.cpp
)

target_link_libraries(impair PRIVATE radio_core radio_warnings)
