// RATCHET (#5230): no test source may hardcode a fixed `/tmp/...` path inside a
// string literal (a Bezel program's data path, a C++ scratch path, ...).
//
// WHY THIS EXISTS. file_io_644 / filestream_645 / deserializer_arrayspan_1358 /
// string_fallback_positive_list_2933 embedded Bezel programs that wrote and read
// fixed paths such as `/tmp/bzt644_wrab2.bin`. Two test processes running the
// same suite concurrently raced on those files (2 spurious failures measured in
// one codegen half), the program-text analogue of #5224's harness-stem
// collisions. CLAUDE.md also says "Scratch never goes in /tmp" (tmpfs with a
// quota). The fix is tests/support/test_data_paths.hpp: tests write
// `@DATA@/<name>` and spliceDataPaths() substitutes a per-process
// wtScratchDir() (built with unique_stem).
//
// WHAT IT FLAGS (comments stripped, line by line): any occurrence of `/tmp/`
// that is not the tail of `/var/tmp/`. ALLOWED: a line carrying the marker
// `5230-allow` (with a reason), and mkstemp/mkdtemp templates (`XXXXXX`), which
// are unique by construction. Test-only; no compiler behaviour is asserted.

#include <filesystem>
#include <fstream>
#include <iostream>
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

// Strip `//` comments (string-aware) and `/* ... */` blocks; `inBlock` carries
// the block-comment state across lines.
std::string stripComments(const std::string& l, bool& inBlock) {
    std::string out;
    bool inStr = false;
    for (std::size_t i = 0; i < l.size(); ++i) {
        if (inBlock) {
            if (l[i] == '*' && i + 1 < l.size() && l[i + 1] == '/') {
                inBlock = false;
                ++i;
            }
            continue;
        }
        if (l[i] == '\\') {
            out += l[i];
            if (i + 1 < l.size()) out += l[++i];
            continue;
        }
        if (l[i] == '"') inStr = !inStr;
        if (!inStr && l[i] == '/' && i + 1 < l.size()) {
            if (l[i + 1] == '/') break;
            if (l[i + 1] == '*') { inBlock = true; ++i; continue; }
        }
        out += l[i];
    }
    return out;
}

bool hasFixedTmp(const std::string& code) {
    for (std::size_t p = code.find("/tmp/"); p != std::string::npos;
         p = code.find("/tmp/", p + 1)) {
        // `/var/tmp/` is a different (scratch-policy-compliant) root.
        if (p >= 4 && code.compare(p - 4, 4, "/var") == 0) continue;
        return true;
    }
    return false;
}

void scanText(const std::string& file, const std::string& text,
              std::vector<Offender>& out) {
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    bool inBlock = false;
    while (std::getline(in, line)) {
        ++lineNo;
        const std::string code = stripComments(line, inBlock);
        if (!hasFixedTmp(code)) continue;
        if (line.find("5230-allow") != std::string::npos) continue;
        if (code.find("XXXXXX") != std::string::npos) continue;
        out.push_back({file, lineNo, line});
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
        if (it->path().filename() == "fixed_tmp_data_path_5230_test.cpp")
            continue;
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

BEZEL_TEST(fixed_tmp_data_path_5230, scanner_flags_fixed_tmp_literals_only) {
    std::vector<Offender> o;
    scanText("a.cpp",
             "    \"File.Delete(\\\"/tmp/bzt644_wrab.bin\\\"); \"\n", o);
    BEZEL_EXPECT_EQ(o.size(), std::size_t(1));
    o.clear();
    scanText("a.cpp", "    std::string p = \"/tmp/bzt4357fs_dd.bin\";\n", o);
    BEZEL_EXPECT_EQ(o.size(), std::size_t(1));
    o.clear();
    scanText("a.cpp",
             "    // \"/tmp/in_a_comment.bin\"\n"
             "    /* \"/tmp/in_a_block.bin\" */\n"
             "    const char* d = \"/var/tmp/bezel-scratch/x\";\n"
             "    char t[] = \"/tmp/bezel_x_XXXXXX\";\n"
             "    auto s = \"/tmp/asan_repro\";  // 5230-allow: ASan trace text\n"
             "    \"@DATA@/bzt644_wrab.bin\"\n",
             o);
    BEZEL_EXPECT_EQ(o.size(), std::size_t(0));
}

BEZEL_TEST(fixed_tmp_data_path_5230,
           no_test_source_hardcodes_a_fixed_tmp_path) {
    const std::string root = ::bezel::test::repoRoot();
    BEZEL_EXPECT_TRUE(!root.empty());
    std::size_t filesScanned = 0;
    const std::vector<Offender> bad = scanTestsTree(root, &filesScanned);
    // An unresolvable root iterates zero entries silently; demand a real walk.
    BEZEL_EXPECT_TRUE(filesScanned > 500);
    for (const Offender& b : bad) {
        std::cout << "    FIXED /tmp path in test source (use "
                     "support/test_data_paths.hpp @DATA@ or unique_stem under "
                     "wtScratchDir; or mark `5230-allow: <reason>`): "
                  << b.file << ":" << b.line << "\n      " << b.text << "\n";
    }
    BEZEL_EXPECT_EQ(bad.size(), std::size_t(0));
}
