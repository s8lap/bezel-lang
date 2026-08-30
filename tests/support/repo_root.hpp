#pragma once

// Repo-root discovery for source-level RATCHET tests — the small family of
// tests that scan tests/ or src/ THEMSELVES (a directory walk, or opening a
// specific file by a repo-relative path) rather than driving the compiler.
// fix #3627.
//
// WHY THIS HEADER EXISTS. Four ratchet files each carried an IDENTICAL
// __FILE__-derived repoRoot() (a fixed "go up 3 path components" climb):
//   tests/support/scratch_path_hygiene_test.cpp
//   tests/support/typeref_holder_scope_escape_3487_test.cpp
//   tests/support/internal_degrade_disposition_snapshot_2924_test.cpp
//   tests/module/semacontext_ast_side_table_snapshot_test.cpp
// #3468's fix (42cb282) diagnosed the SAME defect class one file over
// (native_test_harness.hpp's runtimeSourceDir()): a ccache `base_dir` +
// `hash_dir=false` config rewrites every absolute compiler argument
// (including what __FILE__ expands to) to one RELATIVE TO THE COMPILER'S
// CWD, so __FILE__ compiles to a chain of ".." segments that resolves only
// from the CMake build dir — nowhere else, and in particular not from the
// worktree root gate.sh:71 runs test binaries from.
//
// THE FOUR FILES' FAILURE MODES DIFFER, measured 2026-08-30 (see #3627):
//   * scratch_path_hygiene_test.cpp has NO floor on how much it scanned — a
//     fs::recursive_directory_iterator over an unresolvable root silently
//     iterates ZERO entries (no exception, no ec the caller reads), so a
//     scan meant to visit ~1,760 files visits none, finds zero hits, and the
//     "zero hits expected" assertion PASSES. 0.045s for a nominal
//     ~1,760-file walk is the tell.
//   * typeref_holder_scope_escape_3487_test.cpp and
//     internal_degrade_disposition_snapshot_2924_test.cpp already carry
//     their own "the scan must have found SOMETHING" floor
//     (`filesScanned > 20` / `sites.size() > 22`) precisely because each was
//     written with "an empty walk would pass vacuously" in its own comment —
//     so a bad root already fails LOUD in those two, just not for the
//     precisely-diagnosed reason.
//   * semacontext_ast_side_table_snapshot_test.cpp opens ONE specific file
//     via ifstream and asserts `in.good()`, which is already loud on a bad
//     root for the same reason.
// All four nonetheless shared the SAME defect (an unvalidated, potentially
// wrong-and-silently-accepted root) and the SAME fix shape as #3468's:
// validate before trusting, with an absolute -D as the primary channel. The
// two files with an existing floor keep it as defense-in-depth; centralizing
// the derivation here is what makes scratch_path_hygiene_test.cpp's
// previously-unguarded scan safe, and stops a FIFTH copy of this helper from
// ever being pasted in.
//
// THE FIX mirrors #3468's: an absolute -D (BEZEL_REPO_ROOT_DIR, from
// tests/CMakeLists.txt's ${CMAKE_SOURCE_DIR}) is the primary channel — a -D
// value is not a path argument, so no path-rewriting compiler wrapper
// touches it. The __FILE__ fallback is kept (a TU built outside CMake still
// works) but is now VALIDATED: repoRoot() only accepts a candidate that is
// absolute AND carries landmarks every checkout has (CMakeLists.txt at the
// top, plus docs/Bezel_Language_Spec.md) — an unresolvable relative chain,
// or a path that happens to exist but is NOT this repo, is REJECTED and
// repoRoot() returns "" instead of silently handing back nonsense. Every
// caller already asserts !root.empty() before using it (see the four files
// above), so a validated "" turns into the loud, correctly-diagnosed failure
// CLAUDE.md's "an oracle that cannot observe the effect cannot certify the
// path" calls for, instead of a scan over the wrong directory.

#include <filesystem>
#include <string>
#include <system_error>

namespace bezel::test {

namespace repo_root_detail {

namespace fs = std::filesystem;

// A candidate is accepted as the repo root only if it is absolute and
// carries landmarks every checkout has — this rejects both an unresolvable
// relative "../.." chain AND a path that resolves to some unrelated
// existing directory.
inline bool looksLikeRepoRoot(const fs::path& p) {
    std::error_code ec1, ec2;
    return p.is_absolute() &&
           fs::is_regular_file(p / "CMakeLists.txt", ec1) && !ec1 &&
           fs::is_regular_file(p / "docs" / "Bezel_Language_Spec.md", ec2) &&
           !ec2;
}

}  // namespace repo_root_detail

// Absolute repo root, or "" if it cannot be determined. Callers MUST treat
// "" as a hard failure (BEZEL_EXPECT_TRUE(!root.empty())) — never as "found
// nothing to scan under it" (see the file comment above).
inline std::string repoRoot() {
    namespace fs = std::filesystem;
#ifdef BEZEL_REPO_ROOT_DIR
    {
        const fs::path fromBuild(BEZEL_REPO_ROOT_DIR);
        if (repo_root_detail::looksLikeRepoRoot(fromBuild))
            return fromBuild.lexically_normal().string();
    }
#endif
    // Fallback: <root>/tests/<group>/<this TU> -> <root> (three components:
    // the including TU's own filename, its tests/<group> directory, and
    // tests/ itself). __FILE__ here is THIS header's own path
    // (tests/support/repo_root.hpp), which is at the same depth
    // (tests/<group>/<file>) as every current caller, so the climb is
    // identical regardless of which of the four files included it.
    fs::path p(__FILE__);
    for (int up = 0; up < 3; ++up) {
        if (!p.has_parent_path()) return {};
        p = p.parent_path();
    }
    if (!repo_root_detail::looksLikeRepoRoot(p)) return {};
    return p.lexically_normal().string();
}

}  // namespace bezel::test
