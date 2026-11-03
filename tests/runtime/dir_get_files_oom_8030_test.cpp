// fix #8030 (§31.4, v0.350): the runtime's file-IO translation unit checked
// SOME of its allocations and not others.
//
// ── THE DEFECT ────────────────────────────────────────────────────────────
//
// `src/runtime/bezel_runtime_fileio.cpp` states its own policy in its
// header ("every path — success AND error — must leave *out/*len in a
// defined state … Also hardened: … unchecked malloc() (OOM null-deref via
// `buf[n] = 0`)"), and the §7.3 read paths honoured it
// (`bz_file_read_all_bytes` returns `-ENOMEM`; `bz_file_read_all_lines`
// calls `bz_alloc_fail(...)`). The `Directory.*` and `Path.*` entry points
// did NOT: `bz_dir_get_files` / `bz_dir_get_dirs` (#8030's two names) each
// had an unchecked `strdup` (a null pointer handed to the generated
// program's `strlen`) and an unchecked `malloc` (a NULL-pointer write inside
// the runtime), and the audit behind this fix found ELEVEN more unchecked
// allocations in the same file.
//
// ── WHY THE TEST IS A CODE-LEVEL CHECK ─────────────────────────────────────
//
// The defect is reachable only under an allocation failure, and this project
// does not build a mock allocator (see the note in
// `fileio_read_all_lines_oom_hardening_2237_test.cpp`: "Direct OOM injection
// (LD_PRELOAD malloc hook, or a cgroup OOM trigger) is out of scope here").
// So the regression guard is a SOURCE-LEVEL ratchet over the runtime
// translation unit — the same shape the #2234/#2237 fixes were pinned with —
// PLUS behavioural rows that measure what the fix must not disturb.
//
// The ratchet is NOT vacuous: it reports every allocation site in the file
// that is not (a) assigned to a named variable and (b) null-checked in the
// same function. On the pre-fix source it reports thirteen sites, covering
// fifteen allocation calls (the four #8030 names, plus the eleven the audit
// found); on the fixed source it reports none. The fail-without arm runs this
// exact test against the parent's compiler and source.
//
// ── THE CONVENTION THE RATCHET ENCODES ─────────────────────────────────────
//
// §31.4 (v0.350) makes an allocation failure a TERMINATION and not an
// exception — "the runtime reports the failure and aborts the process.
// Allocation failure is not an exception" — and `bz_alloc_fail`
// (`bezel_runtime_internal.hpp`) is the one funnel every such site uses.
// The single exception in this file is `bz_file_read_all_bytes`, which HAS
// an error code to return and returns `-ENOMEM`; it is checked too, so the
// ratchet does not need to know the difference.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "runtime/bezel_runtime.h"
#include "support/build_paths.hpp"
#include "support/scratch_dir.hpp"
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

// The runtime translation unit under audit. `BEZEL_RUNTIME_SRC_DIR` is the
// build's own absolute source dir (tests/CMakeLists.txt), so this does not
// depend on the working directory of whoever runs the binary.
fs::path fileioSourcePath() {
    return fs::path(BEZEL_RUNTIME_SRC_DIR) / "bezel_runtime_fileio.cpp";
}

bool isIdentChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// The variable an allocation is assigned to: the identifier immediately
// before the line's first `=`. Empty when the line has no assignment (the
// pre-fix `results.push_back(strdup(fullpath));` and `return strdup(...)`
// shapes) — which the ratchet reports as a violation in its own right,
// because an allocation whose result is not held in a named variable cannot
// be null-checked at all.
std::string assignedVariable(const std::string& code) {
    std::size_t eq = code.find('=');
    if (eq == std::string::npos) return {};
    // Walk back over the identifier.
    std::size_t end = eq;
    while (end > 0 && (code[end - 1] == ' ' || code[end - 1] == '\t')) --end;
    std::size_t begin = end;
    while (begin > 0 && isIdentChar(code[begin - 1])) --begin;
    if (begin == end) return {};
    return code.substr(begin, end - begin);
}

// The end of the enclosing function: the next line that is exactly `}` at
// column 0 (this file's style closes every function that way).
std::size_t functionEnd(const std::vector<std::string>& lines,
                         std::size_t from) {
    for (std::size_t i = from; i < lines.size(); ++i) {
        std::string t = lines[i];
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t' ||
                              t.back() == '\r'))
            t.pop_back();
        if (t == "}") return i;
    }
    return lines.size();
}

// Is `var` null-checked between `from` and `to`? The file's style is
// `if (!var) ...`, and `var == nullptr` / `var == NULL` count too.
bool hasNullCheck(const std::vector<std::string>& lines, std::size_t from,
                  std::size_t to, const std::string& var) {
    for (std::size_t i = from; i < to && i < lines.size(); ++i) {
        const std::string& l = lines[i];
        std::size_t pos = 0;
        while ((pos = l.find('!', pos)) != std::string::npos) {
            if (l.compare(pos + 1, var.size(), var) == 0) {
                std::size_t after = pos + 1 + var.size();
                if (after >= l.size() || !isIdentChar(l[after])) return true;
            }
            ++pos;
        }
        if (l.find(var + " == nullptr") != std::string::npos) return true;
        if (l.find(var + " == NULL") != std::string::npos) return true;
    }
    return false;
}

// The ratchet: every allocation in the file must be assigned to a named
// variable AND null-checked in the same function.
std::vector<std::string> uncheckedAllocations(const std::string& src) {
    std::vector<std::string> lines;
    {
        std::istringstream in(src);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }
    static const char* kAllocFns[] = {"malloc(", "calloc(", "realloc(", "strdup("};
    std::vector<std::string> bad;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        // A comment line is not code. (The file's own comment blocks quote
        // the PRE-FIX code, including `malloc(`/`strdup(` — those must not
        // be read as live sites.)
        std::string t = lines[i];
        std::size_t firstNonSpace = t.find_first_not_of(" \t");
        if (firstNonSpace == std::string::npos) continue;
        if (t[firstNonSpace] == '/') continue;
        // Drop a trailing comment.
        std::size_t cmt = t.find("//");
        std::string code = (cmt == std::string::npos) ? t : t.substr(0, cmt);
        bool isAlloc = false;
        for (const char* fn : kAllocFns) {
            if (code.find(fn) != std::string::npos) {
                isAlloc = true;
                break;
            }
        }
        if (!isAlloc) continue;

        std::string var = assignedVariable(code);
        if (var.empty()) {
            bad.push_back("line " + std::to_string(i + 1) + ": allocation not "
                          "assigned to a named variable: " + t);
            continue;
        }
        std::size_t end = functionEnd(lines, i);
        if (!hasNullCheck(lines, i + 1, end, var)) {
            bad.push_back("line " + std::to_string(i + 1) + ": `" + var +
                          "` is not null-checked: " + t);
        }
    }
    return bad;
}

// Read a whole file; empty on failure.
std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The per-process working directory for this test.
const fs::path& workDir() {
    return wtScratchDir("wt-8030-fileio");
}

// A directory with two regular files, one subdirectory, and one dot-file.
fs::path makeFixtureDir() {
    fs::path dir = workDir() / "fixture";
    std::error_code ec;
    fs::create_directories(dir / "sub", ec);
    FILE* f = std::fopen((dir / "alpha.txt").string().c_str(), "wb");
    if (f) std::fclose(f);
    f = std::fopen((dir / "beta.txt").string().c_str(), "wb");
    if (f) std::fclose(f);
    f = std::fopen((dir / ".hidden").string().c_str(), "wb");
    if (f) std::fclose(f);
    return dir;
}

// The `char**` the runtime returns is the CALLER's to release (stdlib §7.4 /
// #8024): the buffer and every entry it holds.
void releaseList(void* arr, int64_t count) {
    if (!arr) return;
    char** v = (char**)arr;
    for (int64_t i = 0; i < count; ++i) std::free(v[i]);
    std::free(v);
}

}  // namespace

// ---------------------------------------------------------------------------
// The ratchet: EVERY allocation in the runtime's file-IO translation unit is
// assigned to a named variable and null-checked in the same function.
//
// RED on the pre-fix source (thirteen sites, covering fifteen allocation
// calls), GREEN on the fixed source.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_fileio_oom_8030, every_allocation_in_the_file_is_checked) {
    const fs::path p = fileioSourcePath();
    BEZEL_EXPECT_TRUE(fs::exists(p));
    if (!fs::exists(p)) return;
    const std::string src = readFile(p);
    BEZEL_EXPECT_TRUE(!src.empty());
    if (src.empty()) return;

    std::vector<std::string> bad = uncheckedAllocations(src);
    if (!bad.empty()) {
        std::string msg;
        for (const std::string& b : bad) msg += "\n  " + b;
        std::fprintf(stderr,
                     "[8030] unchecked allocation(s) in %s:%s\n",
                     p.string().c_str(), msg.c_str());
    }
    BEZEL_EXPECT_TRUE(bad.empty());
}

// ---------------------------------------------------------------------------
// Behavioural row: the fix must not disturb the normal path —
// `Directory.GetFiles` returns the regular files (not the subdirectory, not
// the dot-file) and `Directory.GetDirectories` returns the subdirectory.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_fileio_oom_8030, get_files_and_get_dirs_still_report_the_right_entries) {
    const fs::path dir = makeFixtureDir();
    int64_t nfiles = -1;
    void* files = bz_dir_get_files(dir.string().c_str(), &nfiles);
    BEZEL_EXPECT_TRUE(files != nullptr);
    BEZEL_EXPECT_EQ(nfiles, (int64_t)2);
    if (files && nfiles == 2) {
        char** v = (char**)files;
        bool sawAlpha = false, sawBeta = false, sawSub = false, sawHidden = false;
        for (int64_t i = 0; i < nfiles; ++i) {
            std::string e(v[i]);
            sawAlpha = sawAlpha || e.find("alpha.txt") != std::string::npos;
            sawBeta = sawBeta || e.find("beta.txt") != std::string::npos;
            sawSub = sawSub || e.find("sub") != std::string::npos;
            sawHidden = sawHidden || e.find(".hidden") != std::string::npos;
        }
        BEZEL_EXPECT_TRUE(sawAlpha);
        BEZEL_EXPECT_TRUE(sawBeta);
        BEZEL_EXPECT_FALSE(sawSub);
        BEZEL_EXPECT_FALSE(sawHidden);
    }
    releaseList(files, nfiles);

    int64_t ndirs = -1;
    void* dirs = bz_dir_get_dirs(dir.string().c_str(), &ndirs);
    BEZEL_EXPECT_TRUE(dirs != nullptr);
    BEZEL_EXPECT_EQ(ndirs, (int64_t)1);
    if (dirs && ndirs == 1) {
        char** v = (char**)dirs;
        BEZEL_EXPECT_TRUE(std::string(v[0]).find("sub") != std::string::npos);
    }
    releaseList(dirs, ndirs);
}

// ---------------------------------------------------------------------------
// Behavioural row: an EMPTY directory yields a count of 0 and a NON-NULL
// buffer. The fix allocates at least one element's worth, because
// `malloc(0)` may return nullptr and on these entry points a nullptr already
// means "the directory could not be opened" — a null return for an empty
// directory would be indistinguishable from that.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_fileio_oom_8030, empty_directory_yields_zero_count_and_a_non_null_buffer) {
    fs::path dir = workDir() / "empty";
    std::error_code ec;
    fs::create_directories(dir, ec);
    BEZEL_EXPECT_TRUE(!ec);

    int64_t nfiles = -1;
    void* files = bz_dir_get_files(dir.string().c_str(), &nfiles);
    BEZEL_EXPECT_EQ(nfiles, (int64_t)0);
    BEZEL_EXPECT_TRUE(files != nullptr);
    releaseList(files, nfiles);

    int64_t ndirs = -1;
    void* dirs = bz_dir_get_dirs(dir.string().c_str(), &ndirs);
    BEZEL_EXPECT_EQ(ndirs, (int64_t)0);
    BEZEL_EXPECT_TRUE(dirs != nullptr);
    releaseList(dirs, ndirs);
}

// ---------------------------------------------------------------------------
// Behavioural row: a directory that CANNOT be opened keeps its contract —
// nullptr with a count of 0. That is the shape the fix must NOT change (it
// is why an allocation failure terminates instead of returning nullptr).
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_fileio_oom_8030, unopenable_directory_still_returns_null_with_zero_count) {
    const std::string missing =
        (workDir() / "no_such_dir_8030" / "nested").string();
    int64_t n = -1;
    void* files = bz_dir_get_files(missing.c_str(), &n);
    BEZEL_EXPECT_TRUE(files == nullptr);
    BEZEL_EXPECT_EQ(n, (int64_t)0);

    int64_t m = -1;
    void* dirs = bz_dir_get_dirs(missing.c_str(), &m);
    BEZEL_EXPECT_TRUE(dirs == nullptr);
    BEZEL_EXPECT_EQ(m, (int64_t)0);
}
