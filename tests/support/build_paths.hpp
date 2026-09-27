#pragma once
// Per-build absolute paths the test harness needs (#5010).
//
// These used to arrive as five `-D` string defines on EVERY test TU
// (tests/CMakeLists.txt). A -D value is part of each TU's ccache key, and the
// harness headers expanded the strings into every TU's preprocessed text, so a
// fresh worktree at the same commit missed the cache on all ~2,000 test TUs.
//
// Now the values are `extern const char[]` definitions in ONE generated TU
// (<build>/tests/build_paths.cpp, see tests/CMakeLists.txt). The historical
// macro names are kept as aliases for the constants, so every `#ifdef BEZEL_X`
// and use site keeps working.
//
// #5074: every test TU is compiled with the five names POISONED
// (`-DBEZEL_CORE_LIB_PATH=bezel_include_support_build_paths_hpp`, ...), and
// this header #undef's and redefines them. So `#ifdef BEZEL_X` is always true,
// and a TU that uses one of them WITHOUT including this header fails to
// compile ("use of undeclared identifier
// 'bezel_include_support_build_paths_hpp'") instead of silently compiling an
// #ifdef fallback branch. The poison values are identical in every tree, so
// they do not break the cross-worktree ccache keys.
//
// The values are still ABSOLUTE and still per-build (the #3468 / #3627
// guarantees: never derive these from __FILE__, which ccache base_dir and
// -ffile-prefix-map make relative).

namespace bezel::test::build_paths {
extern const char kRuntimeLibPath[];  // libbezel_runtime.a
extern const char kCoreLibPath[];     // libbezel_core.a
extern const char kBezelcPath[];      // the bezelc driver binary
extern const char kRuntimeSrcDir[];   // <repo>/src/runtime
extern const char kRepoRootDir[];     // <repo>
}  // namespace bezel::test::build_paths

#undef BEZEL_RUNTIME_LIB_PATH
#define BEZEL_RUNTIME_LIB_PATH (::bezel::test::build_paths::kRuntimeLibPath)
#undef BEZEL_CORE_LIB_PATH
#define BEZEL_CORE_LIB_PATH (::bezel::test::build_paths::kCoreLibPath)
#undef BEZELC_PATH
#define BEZELC_PATH (::bezel::test::build_paths::kBezelcPath)
#undef BEZEL_RUNTIME_SRC_DIR
#define BEZEL_RUNTIME_SRC_DIR (::bezel::test::build_paths::kRuntimeSrcDir)
#undef BEZEL_REPO_ROOT_DIR
#define BEZEL_REPO_ROOT_DIR (::bezel::test::build_paths::kRepoRootDir)
