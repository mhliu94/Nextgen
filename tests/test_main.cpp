#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include "core/logging.h"

int main(int argc, char** argv) {
    ng::log::set_level(ng::log::Level::Error);
    doctest::Context context;
    context.applyCommandLine(argc, argv);
    return context.run();
}
