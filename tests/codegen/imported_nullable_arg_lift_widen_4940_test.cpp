// Cross-module imported CALLEE with nullable-VALUE-TYPE parameters widened by
// the caller — issue #4940 (§5.1, v0.307).
//
// REGRESSION PIN. Issue #4940 reports: a `new` call passes an `int?` local to
// a `long?` formal; the callee sees a garbage non-null value (4294967295) when
// the local is null. The HYPOTHESIS in the issue — "the {i8,i32} carrier is
// zero- or sign-extended as a whole into the {i8,i64} slot, instead of being
// re-wrapped" — pinpoints exactly what was wrong.
//
// The local-callee arm of buildOrderedArgs already does the lift-widen
// (#4967), so the issue's own single-module probe now compiles and runs
// correctly. The bug remained in the IMPORTED-callee paths — the method-call
// path at LlvmBackend_Calls2.cpp around the CG-nullable block (the
// post-buildOrderedArgs twin), and the hand-rolled imported-ctor arg path
// at LlvmBackend_Exprs2.cpp that walks `ne.args` directly because an
// imported ctor has no reconstructed AST for buildOrderedArgs to consume.
// Both passed the bare-i32 / `ptr null` cases (the #3133 mirror) but had no
// S? → T? widening, so an `int?` argument reached an imported `long?` formal
// as `{i8, i32}` — `verifyModule` rejected the call outright ("Call
// parameter type does not match function signature"); on the happy path the
// non-null bit read as 1 against the i32 zero and the callee saw
// 4294967295 instead of null.
//
// WHAT THIS FILE PINS. Two cross-module programs: a library declaring a
// class with a ctor and a method whose formal parameters are `long?`, an
// importer that passes `int?` locals. The pre-fix build fails the call
// (LLVM IR verifier abort in bezelc); the post-fix build links and runs to
// the spec's correct output. A loop amplifier catches a one-shot vacuous
// pass (per protocol-test-oracles: a single run can leave the LSan-suppressed
// allocation reachable from a live register, so the row reads green on
// both sides of the fix — the 200-iteration loop below removes that
// ambiguity).
//
// The third case (local twin) is an anti-regression pin for #4967's path: a
// future refactor that drops the local-side widening must NOT break this,
// because the imported-side fix is the same shape and a regression there
// would also regress this case's assumption about the local path.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"


namespace bezel::codegen {
namespace {

// Per-process unique working dir under the shared scratch base (same rationale
// as tests/codegen/imported_record_binary_op_positional_3228_test.cpp's
// wtDir3228: /tmp tmpfs races between fork'd shards).
const std::filesystem::path& wtDir4940() {
    return wtScratchDir("wt-4940");
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

std::pair<int, std::string> runCapture(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>/dev/null").c_str(), "r");
    if (!p) return {-1, out};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

// ── (a) IMPORTED ctor — the issue's exact cross-module shape ──────────────
//
// Lib declares `class B(u: long?, v: int?)` whose ctor prints `$"{u ?? -1},
// {v ?? -1}"`. The importer declares `var a: int? := null; var b: int? := 5`
// and constructs `B(a, b)` AND `B(b, a)` — the second call passes a
// non-null `int?` (5) to the `long?` formal AND a null `int?` to the
// `int?` formal. Pre-fix: bezelc aborts at `verifyModule` with "Call
// parameter type does not match function signature" (`{i8, i32}` supplied
// against `{i8, i64}` declared). Post-fix: the importer's call widens the
// `int?` payload to `long?` correctly, both lines print `-1,5` / `5,-1`,
// the program exits 0.
//
// The 200-iteration loop amplifier (per protocol-test-oracles) defends
// against the vacuous-pass class — a one-shot call leaves the widened
// carrier on a stack slot that LSan sees as still reachable, so a single
// iteration reports clean on both pre- and post-fix code (the call
// correctly widened even pre-fix on the SINGLE iteration that fits the
// verifier's tolerance window). 200 iterations stress the lift enough that
// the pre-fix IR verification aborts deterministically on the FIRST.
BEZEL_TEST(imported_nullable_arg_lift_widen_4940,
           imported_ctor_int_question_to_long_question_spec_5_1) {
    auto dir = wtDir4940();
    std::string stem = unique_stem("bezel_4940a_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module B4940; "
        "public class B4940 "
        "  public constructor(u: long?, v: int?) "
        "    Console.WriteLine($\"{u ?? -1},{v ?? -1}\"); "
        "  end constructor; "
        "end B4940; ");
    writeSource(bSrc.path,
        "module App4940; "
        "import B4940; "
        "class C4940 "
        "  public static method Main(): int "
        "    var a: int? := null; var b: int? := 5; "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      using var d := new B4940(a, b); "
        "      using var e := new B4940(b, a); "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end C4940; ");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d; d.metadataPath = aObj.path; d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;
    auto [code, out] = runCapture(bExe.path);
    BEZEL_EXPECT_EQ(code, 0);
    // 200 iterations of (-1,5) then (5,-1). Expect 200 of each.
    std::string expected;
    for (int i = 0; i < 200; ++i) expected += "-1,5\n5,-1\n";
    BEZEL_EXPECT_EQ(out, expected);
}

// ── (b) IMPORTED instance method — the method-call twin of (a) ───────────
//
// Same shape but the callee is a method (not a ctor). The method-call path
// at LlvmBackend_Calls2.cpp ~11858 has the same CG-nullable mirror
// buildOrderedArgs uses and the same gap for S? → T?. Pre-fix the call site
// emits a `{i8, i32}` against an `{i8, i64}` declared and the verifier
// aborts; post-fix the lift lands and the program runs to spec.
BEZEL_TEST(imported_nullable_arg_lift_widen_4940,
           imported_method_int_question_to_long_question_spec_5_1) {
    auto dir = wtDir4940();
    std::string stem = unique_stem("bezel_4940b_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile bSrc((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module B4940m; "
        "public class B4940m "
        "  public method SetUV(u: long?, v: int?): int "
        "    Console.WriteLine($\"M{u ?? -1},{v ?? -1}\"); "
        "    return 0; "
        "  end SetUV; "
        "end B4940m; ");
    writeSource(bSrc.path,
        "module App4940m; "
        "import B4940m; "
        "class C4940m "
        "  public static method Main(): int "
        "    var a: int? := null; var b: int? := 5; "
        "    using var d := new B4940m(); "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      d.SetUV(a, b); "
        "      d.SetUV(b, a); "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end C4940m; ");

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(ra.success);
    if (!ra.success) return;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d; d.metadataPath = aObj.path; d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;
    auto [code, out] = runCapture(bExe.path);
    BEZEL_EXPECT_EQ(code, 0);
    std::string expected;
    for (int i = 0; i < 200; ++i) expected += "M-1,5\nM5,-1\n";
    BEZEL_EXPECT_EQ(out, expected);
}

// ── (c) LOCAL twin — anti-regression pin for #4967 ────────────────────────
//
// The issue's own single-module probe runs locally and now passes (the
// buildOrderedArgs widening was added by #4967). This case pins the
// LOCAL-SIDE arm independently of the imported-side fix — a future
// refactor that broke #4967 would regress this row even with the
// imported-side fix in place, and the regression would be unmistakable.
BEZEL_TEST(imported_nullable_arg_lift_widen_4940,
           local_ctor_int_question_to_long_question_spec_5_1) {
    auto dir = wtDir4940();
    std::string stem = unique_stem("bezel_4940c_");
    TempFile aSrc((dir / (stem + "_a.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(aSrc.path,
        "module M4940c; "
        "class B4940c "
        "  public constructor(u: long?, v: int?) "
        "    Console.WriteLine($\"{u ?? -1},{v ?? -1}\"); "
        "  end constructor; "
        "end B4940c; "
        "class C4940c "
        "  public static method Main(): int "
        "    var a: int? := null; var b: int? := 5; "
        "    using var d := new B4940c(a, b); "
        "    using var e := new B4940c(b, a); "
        "    return 0; "
        "  end Main; "
        "end C4940c; ");

    bezel::Driver driver;
    bezel::CompileResult rb = driver.compileFile(aSrc.path, bExe.path, {});
    BEZEL_EXPECT_TRUE(rb.success);
    if (!rb.success) return;
    auto [code, out] = runCapture(bExe.path);
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_EQ(out, std::string("-1,5\n5,-1\n"));
}

}  // namespace
}  // namespace bezel::codegen
