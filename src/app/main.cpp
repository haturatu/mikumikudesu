#include "app/application.hpp"

#include "core/log.hpp"

#include <exception>

int main(int argc, char** argv) {
    try {
        dayo::app::Application application(dayo::app::parseOptions(argc, argv));
        return application.run();
    } catch (const std::exception& exception) {
        dayo::log::error("Fatal: ", exception.what());
        return 1;
    } catch (...) {
        dayo::log::error("Fatal: unknown exception");
        return 1;
    }
}
