// fix #2455 (§11.1 / §22.10 / §12.8, v0.235): a class-header base-constructor
// argument list (`class D: Base(99)`) was silently discarded by the parser
// for any class `D` with no primary-constructor parameter list of its own —
// no AST node recorded the args, no diagnostic fired, and the base
// constructed with defaults instead of the written value (confirmed for
// LOCAL bases, not merely cross-module). See
// tests/parser/inherit_header_ctor_args_no_primary_ctor_2455_test.cpp for the
// parser-level diagnostic-text assertions and the full spec-reading
// rationale; this file is the runtime-VALUE regression guard for the
// legitimate spellings the fix must not disturb (protocol: assert on real
// values, not merely "compiles").
//
// Values use ADDITION, not multiplication, to stay clear of `runAot`'s
// 8-bit-masked exit code (WEXITSTATUS) — see tests/README.md.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

bool rejectedAtCompile(const std::string& src) {
    auto r = runAot(src);
    return !r.ran && r.diagnostics.hasErrors();
}

bool diagContains(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}
}  // namespace

// §11.1/§22.10/§12.8, v0.235, #2455: the issue's own confirmed repro,
// end-to-end through the full AOT pipeline (parser reject -> no object
// emitted -> does not run). Before the fix this compiled and RAN, returning
// 0 (the calloc-zeroed default) instead of ever surfacing the dropped 99.
BEZEL_TEST(inherit_header_ctor_args_2455,
           base_arglist_no_primary_ctor_rejected_end_to_end) {
    const std::string src =
        "module M;"
        "public class LocalBase"
        "  private _v: int;"
        "  public constructor(v: int) _v := v; end constructor;"
        "  public property Val: int => _v;"
        "end LocalBase;"
        "public class LocalDerived: LocalBase(99) end LocalDerived;"
        "class C public static method Main(): int"
        "  using var d := new LocalDerived(); return d.Val;"
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(rejectedAtCompile(src));
    BEZEL_EXPECT_TRUE(diagContains(
        src, "declares no primary-constructor parameter list of its own"));
}

// Regression guard: the CORRECT alternative spelling the new diagnostic
// recommends — an EMPTY primary-constructor parameter list `()` — must still
// compile AND run with the real, non-default value. Addition-derived value
// (40 + 59 = 99) so a wrong-but-plausible constant can't coincidentally
// satisfy the assertion.
BEZEL_TEST(inherit_header_ctor_args_2455,
           empty_primary_ctor_still_threads_real_value_spec_11_1) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class LocalBase"
        "  private _v: int;"
        "  public constructor(v: int) _v := v; end constructor;"
        "  public property Val: int => _v;"
        "end LocalBase;"
        "public class LocalDerived(): LocalBase(40 + 59) end LocalDerived;"
        "class C public static method Main(): int"
        "  using var d := new LocalDerived(); return d.Val;"
        "end Main; end C;"), 99);
}

// Regression guard: the OTHER correct alternative — an explicit constructor
// chaining `base(...)` in its body — must still compile and run correctly
// (this path was already correct before #2455; guarded here alongside the
// new rejection so both "the bug" and "the fix" have adjacent coverage).
BEZEL_TEST(inherit_header_ctor_args_2455,
           explicit_ctor_base_chain_still_threads_real_value_spec_12_8) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class LocalBase"
        "  private _v: int;"
        "  public constructor(v: int) _v := v; end constructor;"
        "  public property Val: int => _v;"
        "end LocalBase;"
        "public class LocalDerived: LocalBase"
        "  public constructor(v: int) base(v); end constructor;"
        "end LocalDerived;"
        "class C public static method Main(): int"
        "  using var d := new LocalDerived(40 + 59); return d.Val;"
        "end Main; end C;"), 99);
}

// Regression guard: bare inheritance with NO argument list (the overwhelming
// common case, e.g. #2452's own no-explicit-ctor shape) must remain
// accepted and correct.
BEZEL_TEST(inherit_header_ctor_args_2455,
           bare_inheritance_no_arglist_still_correct) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class LocalBase"
        "  private _v: int := 40 + 59;"
        "  public property Val: int => _v;"
        "end LocalBase;"
        "public class LocalDerived: LocalBase end LocalDerived;"
        "class C public static method Main(): int"
        "  using var d := new LocalDerived(); return d.Val;"
        "end Main; end C;"), 99);
}
