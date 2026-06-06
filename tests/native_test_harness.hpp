#pragma once

// Shared helpers for the "native ASan" test leg used in 19 test files.
//
// The key function is asanRuntimeObject(): it compiles bezel_runtime.cpp once
// with -fsanitize=address and caches the resulting .o path in a function-local
// static. The first caller pays the ~2s compile; every subsequent call returns
// the cached path instantly. Because runAll() drives tests single-threaded, no
// locking is needed.
//
// Usage in each test file:
//   #include "native_test_harness.hpp"
//   (remove the local runtimeSourcePath() and the rtCmd/rtObj compilation step)
//   replace:  std::string rtCmd = "clang++ ... -c " + runtimeSourcePath() + ...
//   with:     const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
//             if (rtObj.empty()) { cleanup(); return -1; }
//
// runtimeSourcePath() is also provided here so each test file can call it if
// it needs to reference the source for any other reason.

#include <cstdlib>
#include <filesystem>
#include <string>

namespace bezel::native_test {

namespace fs = std::filesystem;

// Returns the absolute path to src/runtime/bezel_runtime.cpp, derived from
// this header's __FILE__ location (tests/native_test_harness.hpp → repo root).
inline std::string runtimeSourcePath() {
    fs::path here(__FILE__);          // tests/native_test_harness.hpp
    fs::path root = here.parent_path() // tests/
                        .parent_path(); // repo root
    return (root / "src" / "runtime" / "bezel_runtime.cpp").string();
}

// Returns the path to bezel_runtime.cpp compiled with ASan, or an empty string
// if the compilation fails (toolchain unavailable or error). The object is built
// at most once per process: the result is cached in a function-local static so
// every subsequent call returns the cached path with no recompile.
//
// Compiled with: clang++ -std=c++20 -g -fsanitize=address -pthread -c
// This is the identical flag set used by every test file's runUnderLeakSanitizer.
inline const std::string& asanRuntimeObject() {
    static std::string cached;
    static bool attempted = false;
    if (attempted) return cached;
    attempted = true;

    // Check that clang++ is available.
    if (std::system("clang++ --version > /dev/null 2>&1") != 0) {
        // Toolchain unavailable: leave cached empty so callers SKIP.
        return cached;
    }

    fs::path dir = fs::temp_directory_path();
    std::string rtObj = (dir / "bezel_rt_asan_memoized.o").string();

    std::string rtCmd =
        "clang++ -std=c++20 -g -fsanitize=address -pthread -c " +
        runtimeSourcePath() + " -o " + rtObj + " > /dev/null 2>&1";
    if (std::system(rtCmd.c_str()) != 0) {
        // Compilation failed: leave cached empty so callers SKIP.
        return cached;
    }

    cached = rtObj;
    return cached;
}

} // namespace bezel::native_test
