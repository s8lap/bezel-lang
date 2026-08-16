// Cross-module const string value is NOT truncated (#1225, §2.5 / §22.12).
//
// Bug: an imported `public const Hello: string := "hello"` was read as a
// truncated value (first+last character stripped): "hello"→"ell", "ab"→"".
// Root cause: foldImportedConstText (LlvmBackend_Setup.cpp) stripped the
// surrounding quotes from the const's stored valueText BEFORE passing to
// lowerStringLiteralConst, but processStringCodePoints (called inside
// lowerStringLiteralConst) also strips the delimiter pair itself — causing
// double-stripping.  Fix: pass the full lexeme (with quotes) directly to
// lowerStringLiteralConst, matching the within-module StringLiteralExpr path.
//
// Shapes tested:
//   C1  3-char string "abc"   → length 3
//   C2  5-char string "hello" → length 5  (canonical repro from the issue)
//   C3  2-char string "ab"    → length 2  (was → empty before the fix)
//   C4  11-char string        → length 11

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace {

struct TempFile1225 {
    std::string path;
    explicit TempFile1225(std::string p) : path(std::move(p)) {}
    ~TempFile1225() { std::remove(path.c_str()); }
};

void write1225(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileObj1225(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile main (importing aObj), run, return exit code (-1 on failure).
int importRun1225(const std::string& bSrc, const std::string& bExe,
                  const std::string& aObj) {
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, bSrc, bExe, deps).success) return -1;
    int status = std::system(bExe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Write lib and main to temp files, compile, run, return exit code.
int runConstStringTest(const std::string& tag,
                       const std::string& libSrc,
                       const std::string& mainSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz1225_" + tag + "_" + unique_stem("");

    TempFile1225 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile1225 aObj((dir / (stem + "_a.o")).string());
    TempFile1225 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile1225 bExe((dir / (stem + "_b")).string());

    write1225(aSrc.path, libSrc);
    if (!compileObj1225(aSrc.path, aObj.path)) return -1;
    if (!std::ifstream(aObj.path).good()) return -1;

    write1225(bSrc.path, mainSrc);
    return importRun1225(bSrc.path, bExe.path, aObj.path);
}

}  // namespace

// C1: 3-char const string — basic sanity (was slightly less-affected pre-fix
// because substr(1, 1) = 1 char; real value "abc" → before fix read as "b").
// §22.12 const field; §2.5 cross-module fold.
BEZEL_TEST(xmod_const_string_1225, c1_three_char_const_length_22_12) {
    int got = runConstStringTest("c1",
        "module CLib1225; "
        "public class C "
        "  public const Tag: string := \"abc\"; "
        "end C;",
        "module App; "
        "import CLib1225; "
        "class P public static method Main(): int "
        "  var s := C.Tag; return s.Length; "
        "end Main; end P;");
    BEZEL_EXPECT_EQ(got, 3);
}

// C2: 5-char const string — canonical repro from the issue report.
// "hello" (5 chars) was read as "ell" (3 chars) before the fix.
// §22.12 const field; §2.5 cross-module fold.
BEZEL_TEST(xmod_const_string_1225, c2_five_char_const_hello_length_22_12) {
    int got = runConstStringTest("c2",
        "module CLib1225; "
        "public class C "
        "  public const Hello: string := \"hello\"; "
        "end C;",
        "module App; "
        "import CLib1225; "
        "class P public static method Main(): int "
        "  var s := C.Hello; return s.Length; "
        "end Main; end P;");
    BEZEL_EXPECT_EQ(got, 5);
}

// C3: 2-char const string — worst case pre-fix ("ab" → empty, length 0).
// substr(1, 0) produced an empty string_view before the fix.
// §22.12 const field; §2.5 cross-module fold.
BEZEL_TEST(xmod_const_string_1225, c3_two_char_const_was_empty_before_fix_22_12) {
    int got = runConstStringTest("c3",
        "module CLib1225; "
        "public class C "
        "  public const Short: string := \"ab\"; "
        "end C;",
        "module App; "
        "import CLib1225; "
        "class P public static method Main(): int "
        "  var s := C.Short; return s.Length; "
        "end Main; end P;");
    BEZEL_EXPECT_EQ(got, 2);
}

// C4: 11-char const string "hello world" — multi-word, verifies no off-by-one
// at any position for longer strings.
// §22.12 const field; §2.5 cross-module fold.
BEZEL_TEST(xmod_const_string_1225, c4_eleven_char_const_hello_world_length_22_12) {
    int got = runConstStringTest("c4",
        "module CLib1225; "
        "public class C "
        "  public const Msg: string := \"hello world\"; "
        "end C;",
        "module App; "
        "import CLib1225; "
        "class P public static method Main(): int "
        "  var s := C.Msg; return s.Length; "
        "end Main; end P;");
    BEZEL_EXPECT_EQ(got, 11);
}
