#include "core/log.hpp"

#include <string_view>

namespace {

void usage(std::string_view name) {
    dayo::log::info("usage: ", name, " <input.vdb> <output.dds>");
}

} // namespace

int main(int argc, char** argv) {
    // vdb2dds skeleton: conversion entry point placeholder. Real voxel
    // sampling + BCn/HALF encoding lands here additively; today we only
    // validate arguments and document the contract in tools/fixture_oracle.
    if (argc != 3) {
        usage(argv[0]);
        return 1;
    }
    dayo::log::info("vdb2dds skeleton: input=", argv[1], " output=", argv[2], " (conversion not yet implemented)");
    return 0;
}
