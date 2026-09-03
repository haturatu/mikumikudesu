#include "app/application.hpp"
#include "app/audio_export_command.hpp"

#include "core/log.hpp"

#include <exception>
#include <utility>

int main(int argc, char** argv) {
    try {
        auto options = dayo::app::parseOptions(argc, argv);
        if (options.audioExport)
            return dayo::app::runAudioExport(options);
        dayo::app::Application application(std::move(options));
        return application.run();
    } catch (const std::exception& exception) {
        dayo::log::error("Fatal: ", exception.what());
        return 1;
    } catch (...) {
        dayo::log::error("Fatal: unknown exception");
        return 1;
    }
}
