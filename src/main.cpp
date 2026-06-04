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
    out << "usage: bezelc [--version] [--help] [-c] [-o <output>] "
           "[--import <artifact>]... <source-file>\n"
           "  -c                 compile to a native object (a library, §2.4) "
           "with embedded\n"
           "                     interface metadata; do not link, no `Main` "
           "required\n"
           "  --import <artifact> a dependency artifact (object/exe) whose "
           "`.bezel.meta`\n"
           "                     is loaded for `import` resolution (§2.5); its "
           "object is\n"
           "                     also linked into the executable\n";
}

// Default executable name from the source path: drop the directory and the
// final extension (foo/bar.bzl -> bar). Falls back to "a.out".
std::string defaultOutput(const std::string& input) {
    std::string base = input;
    if (auto slash = base.find_last_of('/'); slash != std::string::npos) {
        base = base.substr(slash + 1);
    }
    if (auto dot = base.find_last_of('.'); dot != std::string::npos && dot > 0) {
        base = base.substr(0, dot);
    }
    return base.empty() ? "a.out" : base;
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty()) {
        printUsage(std::cerr);
        return 2;
    }

    std::string inputPath;
    std::string outputPath;
    // §2.4 / §2.5 (issue #328 inc 3): `-c` emits a library object; `--import`
    // names a dependency artifact whose interface is loaded and whose object is
    // linked. A dependency's metadata and object live in the same artifact, so
    // one path fills both Dependency fields.
    bool compileObjectOnly = false;
    std::vector<bezel::Dependency> deps;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--version") {
            std::cout << kVersion << "\n";
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            printUsage(std::cout);
            return 0;
        }
        if (arg == "-o") {
            if (i + 1 >= args.size()) {
                std::cerr << "bezelc: error: -o requires an argument\n";
                return 2;
            }
            outputPath = args[++i];
            continue;
        }
        if (arg == "-c") {
            compileObjectOnly = true;
            continue;
        }
        if (arg == "--import") {
            if (i + 1 >= args.size()) {
                std::cerr << "bezelc: error: --import requires an argument\n";
                return 2;
            }
            const std::string artifact = args[++i];
            bezel::Dependency dep;
            dep.metadataPath = artifact;
            dep.objectPath = artifact;
            deps.push_back(std::move(dep));
            continue;
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
    if (outputPath.empty()) outputPath = defaultOutput(inputPath);

    bezel::Driver driver;
    const bezel::CompileResult result =
        compileObjectOnly
            ? driver.compileObject(inputPath, outputPath, deps)
            : driver.compileFile(inputPath, outputPath, deps);
    return result.success ? 0 : 1;
}
