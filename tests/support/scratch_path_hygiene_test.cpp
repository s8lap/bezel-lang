// Ratchet: no test source may hardcode a scratch root.
//
// WHY THIS TEST EXISTS. On 2026-08-24 the session tooling on this machine was
// unusable for three sessions. The cause was not a compiler bug: 159 test files
// had each hardcoded their own scratch directory, four of them onto /tmp, and
// none of them ever removed it. /tmp here is a 16 GB tmpfs (RAM) with a
// per-user quota; the debris reached the 12,774 MB limit, after which every
// write to /tmp failed with EDQUOT — including the writes the harness uses to
// capture command output, which is why the failure presented as "bash is
// broken" rather than as a full disk. 148,949 leaked directories were swept
// (41 GB under /var/tmp/bezel-scratch, 12 GB under /tmp).
//
// CLAUDE.md already said scratch must never live in /tmp. A rule that only
// exists in prose gets re-broken by the next person (or agent) who writes a
// test at 2am, and the cost is not a failed test — it is the whole box. So the
// rule is enforced here instead: every test that needs a working directory
// calls wtScratchDir() (support/scratch_dir.hpp), which derives the location
// from one policy and reaps what it creates.
//
// If this test fails, do NOT add your file to an allowlist. Replace the
// hardcoded path with wtScratchDir("<tag>") — that is the whole fix, and it is
// two lines shorter than what it replaces.

#include <filesystem>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>

#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

// <root>/tests/support/<this file>  ->  <root>
std::string repoRoot() {
    std::string p = __FILE__;
    for (int up = 0; up < 3; ++up) {
        const std::size_t slash = p.find_last_of('/');
        if (slash == std::string::npos) return {};
        p.resize(slash);
    }
    return p;
}

bool isCommentLine(const std::string& line) {
    const std::size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos) return true;
    return line.compare(i, 2, "//") == 0 || line.compare(i, 1, "*") == 0;
}

struct Hit {
    std::string file;
    int line;
    std::string text;
};

// The patterns that reintroduce the leak: constructing a path from a literal
// scratch root. Matching on the LITERAL (not on "/tmp" anywhere) keeps this
// from firing on the many tests that legitimately mention /tmp in a comment,
// in Bezel source text, or in an expected diagnostic string.
const char* const kBannedLiterals[] = {
    "path(\"/tmp\")",
    "path(\"/tmp/\")",
    "path(\"/var/tmp\")",
    "path(\"/var/tmp/bezel-scratch\")",
    "\"/var/tmp/bezel-scratch\" /",
};

void scan(const std::string& dir, std::vector<Hit>& hits) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        const fs::path& p = it->path();
        if (p.extension() != ".cpp" && p.extension() != ".hpp") continue;
        // This file names the banned literals in order to ban them; and
        // scratch_dir.hpp is where the one legitimate default lives.
        const std::string name = p.filename().string();
        if (name == "scratch_path_hygiene_test.cpp") continue;
        if (name == "scratch_dir.hpp") continue;

        std::ifstream in(p);
        if (!in) continue;
        std::string line;
        int n = 0;
        while (std::getline(in, line)) {
            ++n;
            if (isCommentLine(line)) continue;
            for (const char* pat : kBannedLiterals) {
                if (line.find(pat) != std::string::npos) {
                    hits.push_back({p.string().substr(dir.size() + 1), n, line});
                    break;
                }
            }
        }
    }
}

}  // namespace

// Spec-adjacent: this enforces the CLAUDE.md build-hygiene rule ("Scratch
// artifacts go in ./scratch/ or /var/tmp/bezel-scratch — never /tmp, which is
// tmpfs (RAM)"), not a language rule, so it cites the operating doc.
BEZEL_TEST(scratch_path_hygiene, no_test_source_hardcodes_a_scratch_root) {
    const std::string root = repoRoot();
    BEZEL_EXPECT_TRUE(!root.empty());
    if (root.empty()) return;

    std::vector<Hit> hits;
    scan(root + "/tests", hits);

    // Print every offender before asserting: the count alone would send the
    // reader hunting, and the fix is per-file.
    for (const Hit& h : hits) {
        std::cout << "    hardcoded scratch root: " << h.file << ":" << h.line
                  << "\n      " << h.text
                  << "\n      use wtScratchDir(\"<tag>\") instead"
                     " (tests/support/scratch_dir.hpp)\n";
    }
    BEZEL_EXPECT_EQ(hits.size(), static_cast<std::size_t>(0));
}
