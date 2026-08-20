# Host tools. `radiobench` is a full protocol peer rather than a test stub, so
# that once the Android app exists you can put a debugger, tcpdump and complete
# tracing on one end of a real call. `impair` is the seeded network impairment
# proxy that makes the benchmark numbers reproducible.
#
# Both are added as the layers they depend on land.
