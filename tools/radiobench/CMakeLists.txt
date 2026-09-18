add_executable(radiobench
  main.cpp
  options.cpp
  util.cpp
  mode_respond.cpp
  mode_ping.cpp
  mode_send.cpp
  mode_wavloop.cpp
)

# impair_scenario, so wavloop reads the same scenario files the proxy does. It
# carries radio_core PUBLIC, so naming radio_core again here would put the same
# archive on the link line twice.
target_link_libraries(radiobench PRIVATE impair_scenario radio_warnings)

# wavloop needs the codec. The mode is compiled either way so the CLI surface
# does not change between builds; without this definition it prints how to turn
# Opus on instead of silently not existing.
if(RADIO_WITH_OPUS)
  target_compile_definitions(radiobench PRIVATE RADIO_HAVE_OPUS=1)
endif()
