#include <iostream>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char *argv[]) {
    std::string path = ".";

    // Find first argument that is not an option (doesn't start with '-')
    for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (!arg.empty() && arg[0] == '-') {
                // skip option
                continue;
            }
            // first non-option argument is the path
            path = arg;
            break;
    }

    try {
        for (const auto &entry : fs::directory_iterator(path)) {
            std::cout << entry.path().filename().string() << "  ";
        }
        std::cout << "\n";
    } catch (const fs::filesystem_error &e) {
        std::cerr << "ls: filesystem error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}