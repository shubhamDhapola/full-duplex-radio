add_executable(radiobench
  main.cpp
  options.cpp
  util.cpp
  mode_respond.cpp
  mode_ping.cpp
  mode_send.cpp
)

target_link_libraries(radiobench PRIVATE radio_core radio_warnings)
