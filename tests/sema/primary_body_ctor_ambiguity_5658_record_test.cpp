// fix #5658 (§22.6 stage 8, v0.326): at `new R(...)` on a local record with
// a primary constructor AND a body constructor, the primary is a §22.6
// candidate on equal footing with the body constructor. When no candidate is
// unambiguously better, the call is ambiguous and is a compile error.
//
// Before the fix, the combined-form record arm returned early at
// ExprTypecheck_Calls.cpp:18081 with no §22.6 ambiguity check, so the call
// compiled clean and codegen silently chose a candidate (the same silent-
// pick the class arm had before fix #5099). The class arm (fix #5099,
// v0.307) calls `reportPrimaryBodyCtorAmbiguity` at line 18224; the record
// arm had no equivalent call.
//
// Scope: LOCAL combined-form records only. Imported combined-form records
// route through `checkImportedNewExprArgs` (ExprTypecheck_Calls.cpp:14802),
// which reads no AST decl and is out of scope here.
//
// FAIL-WITHOUT (MEASURED): without the fix the record repro compiles clean
// and exits 0.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains,
                                            // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"           // runAot
#include "test_harness.hpp"

#include <cstdio>
#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasError(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

void expectRejected(const JitResult& r, const char* diag) {
    if (!hasError(r, diag)) {
        std::fprintf(stderr, "  expected diagnostic containing: %s\n", diag);
        for (const auto& d : r.diagnostics.diagnostics())
            std::fprintf(stderr, "    got: %s\n", d.message.c_str());
    }
    BEZEL_EXPECT_TRUE(hasError(r, diag));
    BEZEL_EXPECT_FALSE(r.ran);
}

} // namespace

// Row 1: reject — co-equal implicit conversions. `int` → `long` and `int` →
// `double` are both implicit per §5.1, and §22.6 ranks all implicit
// conversions co-equal, so neither candidate is unambiguously better. The
// diagnostic reuses the existing `resolveOverload` text, which names both
// candidates with the primary's ` (primary constructor)` suffix.
BEZEL_TEST(primary_body_ctor_ambiguity_5658_record,
           rejects_co_equal_conversions_primary_vs_body_ctor_spec_22_6) {
    expectRejected(
        runAot(
            "module M; "
            "record R(X: long) "
            "  public constructor(y: double) "
            "    X := 1; "
            "  end constructor; "
            "end R; "
            "class P public static method Main(): int "
            "  var r := new R(3); "
            "  return 0; "
            "end Main; end P;"),
        "ambiguous call to `new R`: no overload is unambiguously "
        "better (§22.6)");
}

// Row 2: accept control — non-tying types. Each call has exactly one
// applicable candidate, so both are accepted and select correctly.
BEZEL_TEST(primary_body_ctor_ambiguity_5658_record,
           accepts_non_tying_types_primary_and_body_ctor_spec_22_6) {
    auto r = runAot(
        "module M; "
        "record R(V: int) "
        "  public constructor(s: string) "
        "    V := s.Length; "
        "  end constructor; "
        "  public method GetV(): int => V; "
        "end R; "
        "class P public static method Main(): int "
        "  var a := new R(3); "
        "  var b := new R(\"four\"); "
        "  return a.GetV() * 10 + b.GetV(); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 34);
}

// Row 3: accept control — different arity. §22.5 arity filtering leaves one
// candidate per call.
BEZEL_TEST(primary_body_ctor_ambiguity_5658_record,
           accepts_different_arity_primary_and_body_ctor_spec_22_5) {
    auto r = runAot(
        "module M; "
        "record Point2(X: int, Y: int) "
        "  public constructor(scale: int) "
        "    X := scale; "
        "    Y := scale; "
        "  end constructor; "
        "  public method GetX(): int => X; "
        "  public method GetY(): int => Y; "
        "end Point2; "
        "class P public static method Main(): int "
        "  var a := new Point2(5); "
        "  var b := new Point2(6, 7); "
        "  return a.GetX() * 10 + b.GetX(); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 56);
}
