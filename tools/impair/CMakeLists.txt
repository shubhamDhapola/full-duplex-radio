# The scenario parser is a library rather than part of the executable because
# radiobench reads the same files. The benchmark matrix runs the same scenario
# through the proxy and through `radiobench wavloop`, and those two results are
# only comparable if one parser produced both -- two parsers that agree today
# are two parsers that will disagree after the next key is added.
add_library(impair_scenario STATIC scenario.cpp)
target_include_directories(impair_scenario PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(impair_scenario PUBLIC radio_core PRIVATE radio_warnings)

add_executable(impair main.cpp)

# radio_core arrives through impair_scenario, which exposes radio::sim types in
# its own header and therefore links it PUBLIC. Naming it again here would put
# the same archive on the link line twice, which Apple's ld warns about.
target_link_libraries(impair PRIVATE impair_scenario radio_warnings)
