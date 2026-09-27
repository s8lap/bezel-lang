#pragma once
// Per-build absolute paths the test harness needs (#5010).
//
// These used to arrive as five `-D` string defines on EVERY test TU
// (tests/CMakeLists.txt). A -D value is part of each TU's ccache key, and the
// harness headers expanded the strings into every TU's preprocessed text, so a
// fresh worktree at the same commit missed the cache on all ~2,000 test TUs.
//
// Now the values are `extern const char[]` definitions in ONE generated TU
// (<build>/tests/build_paths.cpp, see tests/CMakeLists.txt), and test TUs get
// only the value-free BEZEL_HAVE_BUILD_PATHS switch. The historical macro names
// are kept as aliases for the constants, so every `#ifdef BEZEL_X` and use site
// keeps working — but ONLY in a TU that includes this header, so every file
// that names one of the macros includes it explicitly (an `#ifdef` in a TU
// that forgot to would silently compile the fallback branch).
//
// The values are still ABSOLUTE and still per-build (the #3468 / #3627
// guarantees: never derive these from __FILE__, which ccache base_dir and
// -ffile-prefix-map make relative).

#ifdef BEZEL_HAVE_BUILD_PATHS

namespace bezel::test::build_paths {
extern const char kRuntimeLibPath[];  // libbezel_runtime.a
extern const char kCoreLibPath[];     // libbezel_core.a
extern const char kBezelcPath[];      // the bezelc driver binary
extern const char kRuntimeSrcDir[];   // <repo>/src/runtime
extern const char kRepoRootDir[];     // <repo>
}  // namespace bezel::test::build_paths

#ifndef BEZEL_RUNTIME_LIB_PATH
#define BEZEL_RUNTIME_LIB_PATH (::bezel::test::build_paths::kRuntimeLibPath)
#endif
#ifndef BEZEL_CORE_LIB_PATH
#define BEZEL_CORE_LIB_PATH (::bezel::test::build_paths::kCoreLibPath)
#endif
#ifndef BEZELC_PATH
#define BEZELC_PATH (::bezel::test::build_paths::kBezelcPath)
#endif
#ifndef BEZEL_RUNTIME_SRC_DIR
#define BEZEL_RUNTIME_SRC_DIR (::bezel::test::build_paths::kRuntimeSrcDir)
#endif
#ifndef BEZEL_REPO_ROOT_DIR
#define BEZEL_REPO_ROOT_DIR (::bezel::test::build_paths::kRepoRootDir)
#endif

#endif  // BEZEL_HAVE_BUILD_PATHS
