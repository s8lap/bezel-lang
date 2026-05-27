// bezelc — the Bezel compiler driver.
//
// Argument handling is intentionally hand-rolled: adopting a CLI-parsing
// dependency is a project-level decision (CLAUDE.md), and the scaffold needs
// only enough to route a file path into the driver.

#include <iostream>
#include <string>
#include <vector>

#include "driver/Driver.hpp"

namespace {

constexpr const char* kVersion =
    "bezelc 0.0.0 (scaffold; no compilation implemented yet)";

void printUsage(std::ostream& out) {
    out << "usage: bezelc [--version] [--help] <source-file>\n";
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty()) {
        printUsage(std::cerr);
        return 2;
    }

    std::string inputPath;
    for (const std::string& arg : args) {
        if (arg == "--version") {
            std::cout << kVersion << "\n";
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            printUsage(std::cout);
            return 0;
        }
        if (!arg.empty() && arg.front() == '-') {
            std::cerr << "bezelc: error: unknown option '" << arg << "'\n";
            printUsage(std::cerr);
            return 2;
        }
        if (!inputPath.empty()) {
            std::cerr << "bezelc: error: more than one input file given\n";
            return 2;
        }
        inputPath = arg;
    }

    if (inputPath.empty()) {
        printUsage(std::cerr);
        return 2;
    }

    bezel::Driver driver;
    const bezel::CompileResult result = driver.compileFile(inputPath);
    return result.success ? 0 : 1;
}
