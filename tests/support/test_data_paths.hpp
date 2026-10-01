// Per-process data paths for Bezel programs embedded in tests (#5230).
//
// WHY. Programs in file_io_644 / filestream_645 / deserializer_arrayspan_1358
// wrote and read FIXED data paths (`/tmp/bzt644_wrab2.bin`, ...) from inside the
// Bezel source. Two test processes running the same suite concurrently raced on
// those files (2 spurious failures measured in one codegen half). This is the
// program-text analogue of #5224's harness-stem collisions, and CLAUDE.md also
// says "Scratch never goes in /tmp".
//
// HOW. Test sources write `@DATA@/<name>` where a path is needed; the one
// splice point (spliceDataPaths) replaces `@DATA@` with a per-process directory
// from wtScratchDir() (`<scratchBase()>/<tag>_<pid>_<n>`, built with
// unique_stem, removed at process exit, reaped if the process is SIGKILLed).
// File names inside the directory keep their per-test distinct names, so
// same-process tests stay independent exactly as before.
//
// The directory is asserted free of `"` and `\` so it splices into a Bezel
// string literal without escaping.

#pragma once

#include <cstdlib>
#include <iostream>
#include <string>

#include "support/scratch_dir.hpp"

namespace bezel::test {

inline std::string dataPathDir(const char* tag) {
    std::string d = wtScratchDir(tag).string();
    if (d.find_first_of("\"\\") != std::string::npos) {
        std::cerr << "test_data_paths: scratch dir needs Bezel escaping: " << d
                  << "\n";
        std::abort();
    }
    return d;
}

// Replace every `@DATA@` in a Bezel program with this process's data dir.
inline std::string spliceDataPaths(std::string program, const char* tag) {
    static const std::string kToken = "@DATA@";
    const std::string dir = dataPathDir(tag);
    for (std::size_t p = program.find(kToken); p != std::string::npos;
         p = program.find(kToken, p + dir.size()))
        program.replace(p, kToken.size(), dir);
    return program;
}

}  // namespace bezel::test
