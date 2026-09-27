// fix #5099 (§22.6 stage 8 / §22.7 / §22.10, v0.307): at `new C(...)` on a
// local class with a primary constructor AND body constructors, the primary is
// a §22.6 candidate on equal footing with the body constructors. When no
// candidate is unambiguously better, the call is ambiguous and is a compile
// error. §22.6 says the compiler "never silently selects one".
//
// Before the fix, sema deferred the whole combined shape to codegen, and
// `ctorForArgs` silently chose the body constructor. `class C(x: int)` beside
// `constructor(y: int) this(y);` made `new C(1)` print "body".
//
// Scope: classes only. The record analogue is claimed by #5100 (peer pool)
// and is not changed here. The declaration-side question (does §11.1's "same
// parameter list" include names?) is #4775 Q1 and is unruled. So this is the
// call-site rule that applies under the currently admitted names-differ shape.
//
// FAIL-WITHOUT (MEASURED, sema change reverted, rest of branch intact): the
// two reject rows compile and run; the four accept rows pass on both sides.

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

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

// The #5099 class, shared by rows 1-3.
const char* kTieClass =
    "module M; "
    "class C(x: int) "
    "  public constructor(y: int) "
    "    this(y + 1); "
    "    Console.WriteLine(\"body\"); "
    "  end constructor; "
    "  public method X(): int => x; "
    "end C; ";

} // namespace

// Row 1: the issue's repro. The primary `(x: int)` and the body `(y: int)` are
// both exact for `1: int`, so neither is unambiguously better: stage 8.
BEZEL_TEST(primary_body_ctor_ambiguity_5099,
           rejects_positional_tie_primary_vs_body_ctor_spec_22_6_stage_8) {
    auto r = runAot(std::string(kTieClass) +
        "class P public static method Main(): int "
        "  var c := new C(1); "
        "  Console.WriteLine(\"made\"); "
        "  return c.X(); "
        "end Main; end P;");
    expectRejected(r, "ambiguous call to `new C`: no overload is unambiguously "
                      "better (§22.6)");
    expectRejected(r, "new C(int) (primary constructor)");
}

// Row 2: naming the primary's parameter binds only the primary (the body
// constructor has no parameter `x`, so §22.5 makes it inapplicable). The call
// is accepted and does not run the body constructor.
// ⚠ Only the selection is asserted, not `c.X()`. The value is wrong on
// origin/main-era compilers independently of this fix: a named argument to
// the primary of a LOCAL combined-form class builds x = 0. That is a separate
// defect.
BEZEL_TEST(primary_body_ctor_ambiguity_5099,
           accepts_named_arg_selecting_primary_spec_22_5) {
    auto r = runAot(std::string(kTieClass) +
        "class P public static method Main(): int "
        "  var c := new C(x := 7); "
        "  Console.WriteLine(\"made\"); "
        "  return 0; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("made\n"));
}

// Row 3: naming the body constructor's parameter binds only the body
// constructor. It chains `this(y + 1)`, so x = 8.
BEZEL_TEST(primary_body_ctor_ambiguity_5099,
           accepts_named_arg_selecting_body_ctor_spec_22_5) {
    auto r = runAot(std::string(kTieClass) +
        "class P public static method Main(): int "
        "  var c := new C(y := 7); "
        "  return c.X(); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 8);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("body\n"));
}

// Row 4: a non-tying overload (`int` primary, `string` body). Each call has
// exactly one applicable candidate, so both are accepted and select correctly.
BEZEL_TEST(primary_body_ctor_ambiguity_5099,
           accepts_non_tying_types_primary_and_body_ctor_spec_22_6) {
    auto r = runAot(
        "module M; "
        "class C(x: int) "
        "  public constructor(s: string) "
        "    this(s.Length); "
        "  end constructor; "
        "  public method X(): int => x; "
        "end C; "
        "class P public static method Main(): int "
        "  var a := new C(3); "
        "  var b := new C(\"four\"); "
        "  return a.X() * 10 + b.X(); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 34);
}

// Row 5: a body constructor of a different arity (the #2362 Widget shape).
// §22.5 arity filtering leaves one candidate per call.
BEZEL_TEST(primary_body_ctor_ambiguity_5099,
           accepts_different_arity_primary_and_body_ctor_spec_22_5) {
    auto r = runAot(
        "module M; "
        "class Widget(id: int) "
        "  public constructor(id: int, note: string) "
        "    this(id); "
        "  end constructor; "
        "  public method Id(): int => id; "
        "end Widget; "
        "class P public static method Main(): int "
        "  var a := new Widget(5); "
        "  var b := new Widget(6, \"n\"); "
        "  return a.Id() * 10 + b.Id(); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 56);
}

// Row 6: both candidates need an implicit conversion (`int` to `long` and
// `int` to `double`). §22.6 ranks all implicit conversions as co-equal, so the
// call is ambiguous. The primary is ranked by the same rules as a body ctor.
BEZEL_TEST(primary_body_ctor_ambiguity_5099,
           rejects_co_equal_conversions_primary_vs_body_ctor_spec_22_6) {
    auto r = runAot(
        "module M; "
        "class C(x: long) "
        "  public constructor(y: double) "
        "    this(1); "
        "  end constructor; "
        "end C; "
        "class P public static method Main(): int "
        "  var c := new C(3); "
        "  return 0; "
        "end Main; end P;");
    expectRejected(r, "ambiguous call to `new C`: no overload is unambiguously "
                      "better (§22.6)");
}
