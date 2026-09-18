# Host tools. `radiobench` is a full protocol peer rather than a test stub, so
# that once the Android app exists you can put a debugger, tcpdump and complete
# tracing on one end of a real call.
#
# `impair` is the seeded network impairment proxy that makes the benchmark
# numbers reproducible.
# impair first: it defines the scenario-parser library radiobench links.
add_subdirectory(impair)
add_subdirectory(radiobench)
