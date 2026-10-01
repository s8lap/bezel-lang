// RATCHET (#5224): every AOT / native test harness file stem must be unique
// ACROSS PROCESSES, not just within one.
//
// WHY THIS EXISTS. ~55 codegen tests named their harness files
// `"bezel_<x>_" + std::to_string(counter++)`: a per-process counter with no
// pid. Every lane and gate leg shares one harness directory (scratchBase(),
// /mnt/fast/aot-harness), so two test processes running the same suite
// concurrently overwrote each other's .o and executable. On a loaded machine
// that produced 30-80 spurious FAILs per full codegen run ("No such file",
// "file truncated", "Text file busy", INFRA link-stage failures), every one of
// which passed when rerun alone. The fix is tests/support/unique_stem.hpp
// (`prefix_<pid>_<atomic counter>`); this test keeps the pattern from coming
// back.
//
// WHAT IT FLAGS (per statement, comments stripped):
//   (a) a string literal beginning "bezel_ / "bz that is concatenated with
//       std::to_string(<counter>++ | ++<counter> | <x>.fetch_add(...)) and the
//       statement mentions neither getpid nor unique_stem; and
//   (b) a path component `/ "bezel_..."` / `/ "bz..."` that is a FIXED literal
//       (no counter at all), which collides the same way between processes.
// Test-only; no compiler behaviour is asserted.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "support/repo_root.hpp"
#include "test_harness.hpp"

namespace {

struct Offender {
    std::string file;
    int line;
    std::string text;
};

// Files that DELIBERATELY reuse a fixed scratch path (their own header comments
// say so: the FAIL-WITHOUT trace of a link/build-failure regression must be
// bit-identical across calls, and each writes only a junk .o it expects to
// fail on). They are support-area tests of the -2 / -3 return codes, not
// AOT-harness stems under a shared suite.
bool deliberatelyFixed(const std::string& file) {
    return file == "native_asan_build_failure_returns_minus_two_4563_test.cpp" ||
           file == "native_asan_link_failure_returns_minus_three_3676_test.cpp";
}

std::string stripLineComment(const std::string& l) {
    bool inStr = false;
    for (std::size_t i = 0; i + 1 < l.size(); ++i) {
        if (l[i] == '\\') { ++i; continue; }
        if (l[i] == '"') inStr = !inStr;
        if (!inStr && l[i] == '/' && l[i + 1] == '/') return l.substr(0, i);
    }
    return l;
}

bool statementIsNonUnique(const std::string& s) {
    if (s.find("getpid") != std::string::npos ||
        s.find("unique_stem") != std::string::npos)
        return false;
    static const std::regex counterConcat(
        R"re("(bezel|bz)[A-Za-z0-9_]*"[^;]*to_string\(\s*(\+\+\s*\w+|\w+\s*\+\+|[\w.]+\.fetch_add))re");
    static const std::regex fixedPathLit(
        R"re(/\s*"(bezel_|bz)[0-9][A-Za-z0-9_]*(\.[A-Za-z]+)?")re");
    return std::regex_search(s, counterConcat) ||
           std::regex_search(s, fixedPathLit);
}

void scanText(const std::string& file, const std::string& text,
              std::vector<Offender>& out) {
    std::istringstream in(text);
    std::string line, stmt;
    int lineNo = 0, stmtStart = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        const std::string code = stripLineComment(line);
        if (stmt.empty()) stmtStart = lineNo;
        stmt += code + "\n";
        if (code.find(';') != std::string::npos ||
            code.find('{') != std::string::npos ||
            code.find('}') != std::string::npos) {
            if (statementIsNonUnique(stmt)) {
                std::string one = stmt;
                for (char& c : one) if (c == '\n') c = ' ';
                out.push_back({file, stmtStart, one});
            }
            stmt.clear();
        }
    }
}

std::vector<Offender> scanTestsTree(const std::string& root,
                                    std::size_t* filesScanned) {
    namespace fs = std::filesystem;
    std::vector<Offender> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root + "/tests", ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = it->path().extension().string();
        if (ext != ".cpp" && ext != ".hpp") continue;
        // This file quotes the forbidden shapes in its own self-test.
        if (it->path().filename() == "harness_stem_uniqueness_5224_test.cpp")
            continue;
        if (deliberatelyFixed(it->path().filename().string())) continue;
        std::ifstream in(it->path());
        if (!in.good()) continue;
        std::ostringstream ss;
        ss << in.rdbuf();
        ++*filesScanned;
        scanText(it->path().string().substr(root.size() + 1), ss.str(), out);
    }
    return out;
}

}  // namespace

// The scanner itself: the pre-fix shapes are flagged, the fixed shapes are not.
BEZEL_TEST(harness_stem_uniqueness_5224, scanner_flags_the_prefix_shapes) {
    std::vector<Offender> o;
    scanText("a.cpp",
             "    std::string stem = \"bezel_taskcomb_\" + std::to_string(counter++);\n",
             o);
    BEZEL_EXPECT_EQ(o.size(), std::size_t(1));
    o.clear();
    scanText("a.cpp",
             "    std::string stem = \"bz712_lsan_\" +\n"
             "        std::to_string(++ctr);\n",
             o);
    BEZEL_EXPECT_EQ(o.size(), std::size_t(1));
    o.clear();
    scanText("a.cpp",
             "    auto p = (dir / \"bezel_506_clean_out\").string();\n", o);
    BEZEL_EXPECT_EQ(o.size(), std::size_t(1));
    o.clear();
    scanText("a.cpp",
             "    std::string stem = unique_stem(\"bezel_taskcomb\");\n"
             "    std::string s2 = \"bezel_3550_\" + std::to_string(getpid()) +\n"
             "        \"_\" + std::to_string(counter++);\n"
             "    // \"bezel_x_\" + std::to_string(counter++) in a comment\n",
             o);
    BEZEL_EXPECT_EQ(o.size(), std::size_t(0));
}

BEZEL_TEST(harness_stem_uniqueness_5224,
           no_test_names_harness_files_without_a_pid_unique_stem) {
    const std::string root = ::bezel::test::repoRoot();
    BEZEL_EXPECT_TRUE(!root.empty());
    std::size_t filesScanned = 0;
    const std::vector<Offender> bad = scanTestsTree(root, &filesScanned);
    // An unresolvable root iterates zero entries silently; demand a real walk.
    BEZEL_EXPECT_TRUE(filesScanned > 500);
    for (const Offender& b : bad) {
        std::cout << "    NON-UNIQUE harness stem (use "
                     "support/unique_stem.hpp unique_stem(\"<prefix>\")): "
                  << b.file << ":" << b.line << "\n      " << b.text << "\n";
    }
    BEZEL_EXPECT_EQ(bad.size(), std::size_t(0));
}
