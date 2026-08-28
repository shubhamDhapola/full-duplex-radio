#include <cstdio>

#include "modes.hpp"
#include "options.hpp"
#include "util.hpp"

int main(int argc, char** argv) {
  const auto options = radiobench::parse_options(argc, argv);
  if (!options) {
    radiobench::print_usage(stderr);
    return 2;
  }
  if (options->help) {
    radiobench::print_usage(stdout);
    return 0;
  }

  radiobench::install_signal_handlers();

  switch (options->mode) {
    case radiobench::Options::Mode::Respond:
      return radiobench::run_respond(*options);
    case radiobench::Options::Mode::Ping:
      return radiobench::run_ping(*options);
    case radiobench::Options::Mode::Send:
      return radiobench::run_send(*options);
    case radiobench::Options::Mode::None:
      break;
  }

  radiobench::print_usage(stderr);
  return 2;
}
