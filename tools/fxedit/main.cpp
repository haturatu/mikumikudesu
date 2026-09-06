#include "core/log.hpp"

#include <string_view>

namespace {

void usage(std::string_view name) {
    dayo::log::info("usage: ", name, " [--check] <project.dayo>");
}

} // namespace

int main(int argc, char** argv) {
    // fxedit skeleton: headless project check + keyframe summary entry point.
    // Full ImGui editing stays in the native app; this tool only validates
    // that a project/motion file loads without touching GPU handles.
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    std::string_view mode = argv[1];
    if (mode == "--help" || mode == "-h") {
        usage(argv[0]);
        return 0;
    }
    dayo::log::info("fxedit skeleton: input=", argv[argc - 1], " (no mutation performed)");
    return 0;
}
