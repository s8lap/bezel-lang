// fix #3216 (§22.12 const rules / §22.5 default-arg materialization / §2.5
// dependency resolution; v0.254) — an IMPORTED negative integer constant read 0.
//
// WHAT THE DEFECT WAS. The wire was right and the reader dropped the sign.
//
// `ModuleMetadataBuilder::renderConstExpr` exports a rendered VALUE, not a
// lexeme: `:= -3` is an `ast::UnaryExpr` over an `IntLiteralExpr`, matches no
// literal arm, and falls through to `sema::foldIntConst`, which folds it and
// renders `std::to_string(-3)` = `"-3"` (#2898's "FOLD, don't render"). So
// `MetaMember::valueText` carried `-3` correctly.
//
// `Emitter::foldImportedConstText` (LlvmBackend_Setup.cpp) then admitted a
// leading sign in its guard —
//
//     if (!((c0 >= '0' && c0 <= '9') || c0 == '+' || c0 == '-')) return nullptr;
//
// — and handed the WHOLE string to `bezel::common::parseIntLiteral`, which
// decodes a grammar-A.8 lexeme and A.8 lexemes are UNSIGNED. Its own header says
// so ("the sign of a negative literal is a separate UnaryExpr") and depends on
// it to cover [0, 2^64) with `strtoull`. Handed `"-3"` it broke out of the digit
// loop on the first character, found `digits.empty()`, and returned **0**.
//
// The guard therefore admitted exactly the inputs the parser could not handle,
// and the result was a silent wrong value with no diagnostic anywhere.
//
// MEASURED PRE-FIX on `main` @ 82837ec:
//
//     imported  K.NEG   (`public const NEG: int := -3;`)   -> 0   (twin: -3)
//     imported  K.NEGB  (`:= -7` as `sbyte`)               -> 0   (twin: -7)
//     imported  K.POS   (`:= 3`)                           -> 3   (twin:  3)
//     imported  Widths.S(0) with `s: short := -3` omitted  -> 0   (twin: -3)
//
// The positive row is what makes the defect invisible in review: the common case
// is right, and only the sign is lost.
//
// THE FIX splits the sign off before decoding the magnitude and negates in the
// UNSIGNED domain (`~mv + 1`), because the magnitude of `long.MinValue` is 2^63,
// which has no positive `std::int64_t` and would make `-v` signed-overflow UB.
// Teaching `parseIntLiteral` about a sign would have been wrong: it is the ONE
// decoder for an A.8 LEXEME (#2936 exists because there were two), and A.8
// lexemes have no sign — the two spellings coincide only for non-negative
// numbers.
//
// FOUND WHILE BUILDING #3178, whose §22.5 default-argument materialization goes
// through this same function, so a negative default filled 0. That is the fourth
// row above and it is included here rather than in #3178's file because the
// defect is `foldImportedConstText`'s, not the call path's — the const rows fail
// without #3178 applied at all.
//
// ⚠ NOT COVERED, and deliberately: a negative `long` const reads 4294967256 in
// the SINGLE-MODULE twin too (the negation is emitted at i32 and zero-extended).
// That is a local defect, filed as #3217, and a row asserting it here would be
// asserting someone else's bug. `long` is therefore absent from kLib3216.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile3216 {
    std::string path;
    explicit TempFile3216(std::string p) : path(std::move(p)) {}
    ~TempFile3216() { std::remove(path.c_str()); }
};

void writeSource3216(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// The declarations under test, shared verbatim by the dependency and the
// single-module twin. Every constant is read back through `int(...)` so one
// exit code carries the value; every expected value is offset into 1..255 by
// the reader so a 0 result (the pre-fix answer for every negative row) is
// distinguishable from every correct one.
const char* kDecls3216 =
    "public class K "
    "  public const NEG: int := -3; "
    "  public const POS: int := 3; "
    "  public const NEGB: sbyte := -7; "
    "  public const NEGS: short := -300; "
    "  public const ZERO: int := 0; "
    "end K; "
    "public class Widths "
    "  public static method S(n: int, s: short := -3): int => n + int(s); "
    "end Widths;";

std::string libSource3216() {
    return std::string("module NegConstLib3216; ") + kDecls3216;
}

std::string appSource3216(const std::string& body, const std::string& tag) {
    return "module NegConstApp3216" + tag + "; import NegConstLib3216; "
           "class C " + body + " end C;";
}

std::string sameModuleProgram3216(const std::string& body,
                                  const std::string& tag) {
    return "module NegConstCtl3216" + tag + "; " + kDecls3216 + " class C " +
           body + " end C;";
}

// Returns the child's exit status, -1 on a compile/link/run failure, or -2 when
// the dependency itself could not be produced (no toolchain).
int runImported3216(const std::string& body, const std::string& tag) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3216_" + tag + "_" + std::to_string(std::rand());

    TempFile3216 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3216 aObj((dir / (stem + "_a.o")).string());
    writeSource3216(aSrc.path, libSource3216());
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile3216 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3216 bExe((dir / (stem + "_b")).string());
    writeSource3216(bSrc.path, appSource3216(body, tag));

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!driver.compileFile(bSrc.path, bExe.path, deps).success) return -1;
    if (!std::ifstream(bExe.path).good()) return -1;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(bExe.path.c_str(), bExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int runSingleModule3216(const std::string& src, const std::string& tag) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3216ctl_" + tag + "_" + std::to_string(std::rand());
    TempFile3216 sSrc((dir / (stem + ".bzl")).string());
    TempFile3216 sExe((dir / stem).string());
    writeSource3216(sSrc.path, src);

    bezel::Driver driver;
    if (!driver.compileFile(sSrc.path, sExe.path, {}).success) return -1;
    if (!std::ifstream(sExe.path).good()) return -1;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(sExe.path.c_str(), sExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string mainReturning3216(const std::string& expr) {
    return "  public static method Main(): int "
           "    return " + expr + "; "
           "  end Main;";
}

// Assert that the imported program and its character-identical single-module
// twin produce the SAME value, and that the value is the expected one. Asserting
// only "they agree" would pass if both were 0; asserting only the imported value
// would not show the defect is cross-module. Both are needed.
void expectSameValue3216(const std::string& expr, const std::string& tag,
                         int expected) {
    int imp = runImported3216(mainReturning3216(expr), tag);
    if (imp == -2) return;  // no toolchain for the dependency
    int ctl = runSingleModule3216(
        sameModuleProgram3216(mainReturning3216(expr), tag), tag + "c");
    BEZEL_EXPECT_EQ(ctl, expected);
    BEZEL_EXPECT_EQ(imp, expected);
}

}  // namespace

// ===========================================================================
// T1 — §22.12: a negative `int` const. `100 + K.NEG` is 97; the pre-fix answer
// was 100 (the const read 0). The offset keeps the value in 1..255 and makes
// "read as zero" a DIFFERENT number rather than an absent one.
// ===========================================================================
BEZEL_TEST(xmod_negative_const_3216, t1_imported_negative_int_const_22_12) {
    expectSameValue3216("100 + K.NEG", "t1", 97);
}

// T1b — the POSITIVE const on the same class. This is the row that made the
// defect survive review: the common case was always right.
BEZEL_TEST(xmod_negative_const_3216, t1b_imported_positive_int_const_22_12) {
    expectSameValue3216("100 + K.POS", "t1b", 103);
}

// T1c — a const whose value genuinely IS zero must still read zero. Without it,
// a "fix" that mapped an unparseable text to some sentinel would pass T1.
BEZEL_TEST(xmod_negative_const_3216, t1c_imported_zero_const_22_12) {
    expectSameValue3216("100 + K.ZERO", "t1c", 100);
}

// ===========================================================================
// T2 — the narrower signed widths, where the sign bit is not the 32nd. `sbyte`
// and `short` each go through `lowerPrimitive(rt->primitive)`, so a sign lost
// before the constant is built is lost at that width too.
// ===========================================================================
BEZEL_TEST(xmod_negative_const_3216, t2_imported_negative_sbyte_const_22_12) {
    expectSameValue3216("100 + int(K.NEGB)", "t2", 93);
}

BEZEL_TEST(xmod_negative_const_3216, t2b_imported_negative_short_const_22_12) {
    expectSameValue3216("400 + int(K.NEGS)", "t2b", 100);
}

// ===========================================================================
// T3 — §22.5: the same reader materializes an omitted default argument (#3178),
// so a NEGATIVE default was filled with 0. `Widths.S(100)` is 97.
//
// This row needs BOTH fixes: #3178 to admit the omission at all, and #3216 to
// fill it with the declared value rather than 0.
// ===========================================================================
BEZEL_TEST(xmod_negative_const_3216, t3_imported_negative_default_arg_22_5) {
    expectSameValue3216("Widths.S(100)", "t3", 97);
}

// T3b — the supplied spelling of the same call, as the control that the method
// itself is fine and only the default's materialization was at fault.
BEZEL_TEST(xmod_negative_const_3216, t3b_imported_negative_default_supplied_22_5) {
    expectSameValue3216("Widths.S(100, -3)", "t3b", 97);
}

}  // namespace bezel::module
