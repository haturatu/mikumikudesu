#include "core/log.hpp"

#include <string_view>

namespace {

void usage(std::string_view name) {
    dayo::log::info("usage: ", name, " [--list] [--run <suite>]");
}

} // namespace

int main(int argc, char** argv) {
    // Capability suite stub: enumerates which fixtures/oracles exist and
    // exits 0 when the harness itself runs. Real comparisons plug into the
    // solver tolerance helpers (MotionSolver::compare*) additively.
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--list") {
            dayo::log::info("suites: solver-smoke physics-fixed hdr-roundtrip sequence-parse cache-determinism");
            return 0;
        }
        if (argument == "--help" || argument == "-h") {
            usage(argv[0]);
            return 0;
        }
    }
    dayo::log::info("capability_suite stub: use --list to enumerate suites");
    return 0;
}
