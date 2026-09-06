#include "core/log.hpp"
#include "core/sequence_path.hpp"

#include <string_view>

namespace {

void usage(std::string_view name) {
    dayo::log::info("usage: ", name, " <output-dir> <prefix> <start> <digits> <ext> <count>");
}

} // namespace

int main(int argc, char** argv) {
    // sequence_movie skeleton: validates SequencePathSpec parsing/formatting
    // and proves the bounded OutputQueue contract without encoding frames.
    if (argc != 7) {
        usage(argv[0]);
        return 1;
    }
    dayo::core::SequencePathSpec spec;
    spec.prefix = argv[2];
    try {
        spec.start = static_cast<std::uint32_t>(std::stoul(argv[3]));
        spec.digits = static_cast<std::uint32_t>(std::stoul(argv[4]));
    } catch (...) {
        dayo::log::error("sequence_movie: invalid start/digits");
        return 1;
    }
    spec.extension = argv[5];
    std::uint32_t count = 0;
    try {
        count = static_cast<std::uint32_t>(std::stoul(argv[6]));
    } catch (...) {
        dayo::log::error("sequence_movie: invalid count");
        return 1;
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto path = dayo::core::formatSequencePath(argv[1], spec, spec.start + index);
        dayo::log::debug("sequence_movie frame path: ", path.string());
    }
    dayo::log::info("sequence_movie skeleton: planned ", count, " frames in ", argv[1]);
    return 0;
}
