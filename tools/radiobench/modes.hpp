#pragma once

#include "options.hpp"

namespace radiobench {

int run_respond(const Options& options);
int run_ping(const Options& options);
int run_send(const Options& options);

}  // namespace radiobench
