#pragma once

#include "app/application_options.hpp"

namespace dayo::app {

[[nodiscard]] Options parseOptions(int argc, char** argv);

} // namespace dayo::app
