#include "koi_pond_port.h"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    bool debug_touch = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--debug") {
            debug_touch = true;
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            std::cerr << "Usage: " << argv[0] << " [--debug]" << '\n';
            return 1;
        }
    }

    return koi_pond_run(debug_touch);
}
