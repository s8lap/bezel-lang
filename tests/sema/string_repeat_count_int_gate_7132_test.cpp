// fix #7132 — `string.Repeat`'s COUNT parameter is not type-gated, so a `uint`
// count is admitted even though §5.1 makes `uint → int` NEVER implicit — and
// after the #7104 byte-widening fix the program is SILENTLY WRONG where it
// used to crash loudly.
//
// THE DEFECT. stdlib §9.6 declares `Repeat(c: char, count: int): string` —
// the count is an `int`. A primitive `string` method has no MethodSymbol, so
// the ordinary per-argument assignability check (checkCallArity →
// checkArgsAgainstParams → checkAssignable) never fires for it. The
// built-in-string argument helper `checkBuiltinStringCallArgs` exists to
// supply exactly that check for the primitive-`string` members, and it keeps
// an `intPos` set of the parameters stdlib §9.6 declares as `int`
// (Substring / Remove / Insert / PadLeft / PadRight / PadCenter / AsSpan /
// IndexOf) — but `Repeat`'s count was NOT in that set, so it was never
// checked at all.
//
// MEASURED pre-fix on this tree:
//   `var b: uint := 4000000000; "x".Repeat(b).Length` → COMPILED, printed
//   `-294967296` (a silent wrong value).
//   `var b: uint := 4000000000; string.Repeat('x', b).Length` → COMPILED,
//   printed `0` (a silent wrong value).
//   `var n: long := 3; "x".Repeat(n)` → COMPILED, printed `3` — a `long`
//   is not implicitly convertible to an `int` either (§5.1's table has no
//   `long → int` row).
//   `"x".Repeat(2147483648L)` → COMPILED, printed `-2147483648`.
//   `var d: double := 3.0; "x".Repeat(d)` → the compiler CRASHED out of
//   codegen with "SExt only operates on integer" / "IR verification failed
//   (malformed IR emitted)".
//
// THE GATE. `Repeat`'s count is put on the SAME implicitly-convertible check
// the PAD/SPAN parameters already use: `checkAssignable` against `int`. That
// matters because §5.1's CONSTANT-NARROWING clause rides on it
// (`checkAssignable` runs `narrowIntConstantToTarget` first): an integer
// COMPILE-TIME CONSTANT converts by VALUE to any integer target whose range
// contains it, ACROSS signedness. So `const N: long := 3L` — a `long`
// constant whose value fits `int`'s range — is still well-typed and still
// compiles, while `2147483648L` (a constant that does NOT fit) and
// `var n: long := 3` (not a constant at all) are compile errors.
//
// PORTED by fix #7706 (stdlib §9.6, v0.350): the INSTANCE spelling no longer
// exists (`Repeat` is declared only as a static member), so the rows named
// `*_static_string_unit_*` below drive the declared static
// `Repeat(unit: string, count: int)` -- the count at position 1, gated at the
// static site -- with the same counts the instance rows used. Everything
// below that says "instance" describes the pre-#7706 history of those rows.
//
// BOTH SPELLINGS. The instance `"x".Repeat(count)` puts the count at
// position 0 and is gated in `checkBuiltinStringCallArgs`'s `intPos` set.
// The static `string.Repeat(c, count)` puts it at position 1 and is gated at
// the static-primitive argument check in `typeCall` — the static callee's
// target is the TYPE NAME `string`, which carries no value exprType, so the
// instance helper's receiver guard excludes it by construction. Both report
// the SAME diagnostic, and the rows below assert that they do.
//
// THE DIAGNOSTIC. §5.1's own wording, so a reader knows WHICH sub-rule
// rejected: the signed/unsigned bullet for a `uint`/`ulong` count, the
// constant-narrowing range error for an out-of-range constant.
//
// THE CONTROLS ARE THE POINT. This is a gate ADDED to a parameter, so the
// live risk is over-rejection of the counts §5.1 *does* admit: `int`,
// `byte`, `short`, `char`, a literal, and — the one the constant-narrowing
// clause exists for — a `long` CONSTANT whose value fits `int`'s range.
// Every control asserts a VALUE, never "it compiled".

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// A bare `module M; class C ... end C;` shell — the string members are
// compiler-intrinsic, so no imports or hierarchy are required.
std::string prog(const std::string& mainBody) {
    return "module M;\nclass C\n  public static method Main(): int\n" +
           mainBody + "\n  end Main;\nend C;\n";
}

// True iff SOME error diagnostic is the §5.1 "not implicitly convertible"
// wording for the `string.Repeat` count.
bool notConvertible(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("the argument to `string.Repeat` has a type that "
                           "is not implicitly convertible to the target type "
                           "(§5.1)") != std::string::npos)
            return true;
    return false;
}

// True iff the §5.1 rejection names the SIGNED/UNSIGNED never-implicit
// bullet (§5.1's own example is `uint → int`).
bool signedUnsignedReason(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("between signed and unsigned types of the same or "
                           "narrower width is never implicit") !=
                std::string::npos)
            return true;
    return false;
}

// True iff the §5.1 rejection names the CONSTANT-NARROWING range error.
bool constantRangeReason(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("a constant whose value does not fit") !=
                std::string::npos)
            return true;
    return false;
}

// True iff the pre-existing "requires a `char` (int) count" diagnostic is
// present (the static site's own wording for a NON-integer-like count).
bool requiresCharCount(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("'string.Repeat' requires a `char` (int) count") !=
                std::string::npos)
            return true;
    return false;
}

}  // namespace

// ── REJECT — the INSTANCE spelling, `uint` count ──────────────────────────
// MEASURED pre-fix: compiled, and printed `-294967296` (a silent wrong
// value). §5.1's never-implicit bullet names `uint → int` as its example.
BEZEL_TEST(sema_string_repeat_count_gate_7132, reject_static_string_unit_uint_count) {
    auto r = runAot(prog(
        "    var b: uint := 4000000000;\n"
        "    Console.WriteLine(String.Repeat(\"x\", b).Length);\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(notConvertible(r));
    BEZEL_EXPECT_TRUE(signedUnsignedReason(r));
}

// ── REJECT — the INSTANCE spelling, `ulong` count ──────────────────────────
BEZEL_TEST(sema_string_repeat_count_gate_7132, reject_static_string_unit_ulong_count) {
    auto r = runAot(prog(
        "    var b: ulong := 3;\n"
        "    Console.WriteLine(String.Repeat(\"x\", b).Length);\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(notConvertible(r));
    BEZEL_EXPECT_TRUE(signedUnsignedReason(r));
}

// ── REJECT — the INSTANCE spelling, a NON-CONSTANT `long` count ───────────
// §5.1's table has no `long → int` row, and the constant-narrowing clause
// does not reach a run-time value ("the rule applies to constants only").
// MEASURED pre-fix: compiled, and printed `3`.
BEZEL_TEST(sema_string_repeat_count_gate_7132, reject_static_string_unit_long_var_count) {
    auto r = runAot(prog(
        "    var n: long := 3;\n"
        "    Console.WriteLine(String.Repeat(\"x\", n).Length);\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(notConvertible(r));
}

// ── REJECT — the INSTANCE spelling, a constant that does NOT fit `int` ────
// MEASURED pre-fix: compiled, and printed `-2147483648` (a silent
// truncation). §5.1: "A constant whose value does not fit the target's range
// is a compile error at the conversion site — never a silent truncation."
BEZEL_TEST(sema_string_repeat_count_gate_7132,
           reject_static_string_unit_out_of_range_constant) {
    auto r = runAot(prog(
        "    Console.WriteLine(String.Repeat(\"x\", 2147483648L).Length);\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(notConvertible(r));
    BEZEL_EXPECT_TRUE(constantRangeReason(r));
}

// ── REJECT — the `string`-unit STATIC spelling, a `double` count ──────────
// MEASURED pre-fix (INSTANCE spelling `"x".Repeat(d)`, removed by fix #7706,
// stdlib §9.6 v0.350): the compiler CRASHED out of codegen ("SExt only
// operates on integer" / "IR verification failed (malformed IR emitted)").
// The row now pins the declared static `Repeat(unit: string, count: int)`
// with the same non-integer count: a clean rejection by the static site's own
// wording, never a codegen crash.
BEZEL_TEST(sema_string_repeat_count_gate_7132,
           reject_static_string_unit_double_count) {
    auto r = runAot(prog(
        "    var d: double := 3.0;\n"
        "    Console.WriteLine(string.Repeat(\"x\", d).Length);\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(requiresCharCount(r));
}

// ── REJECT — the STATIC spelling, `uint` count ────────────────────────────
// The static `string.Repeat(c, count)` has the same ungated count. Its
// callee target is the TYPE NAME `string` (no value exprType), so the
// instance helper's receiver guard excludes it — the static site carries its
// own gate. MEASURED pre-fix: compiled, and printed `0`.
BEZEL_TEST(sema_string_repeat_count_gate_7132, reject_static_uint_count) {
    auto r = runAot(prog(
        "    var b: uint := 4000000000;\n"
        "    Console.WriteLine(string.Repeat('x', b).Length);\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(notConvertible(r));
    BEZEL_EXPECT_TRUE(signedUnsignedReason(r));
}

// ── REJECT — the STATIC spelling, `ulong` count ───────────────────────────
BEZEL_TEST(sema_string_repeat_count_gate_7132, reject_static_ulong_count) {
    auto r = runAot(prog(
        "    var b: ulong := 3;\n"
        "    Console.WriteLine(string.Repeat('x', b).Length);\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(notConvertible(r));
    BEZEL_EXPECT_TRUE(signedUnsignedReason(r));
}

// ── REJECT — the STATIC spelling, a NON-CONSTANT `long` count ────────────
BEZEL_TEST(sema_string_repeat_count_gate_7132, reject_static_long_var_count) {
    auto r = runAot(prog(
        "    var n: long := 3;\n"
        "    Console.WriteLine(string.Repeat('x', n).Length);\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(notConvertible(r));
}

// ── REJECT — the STATIC spelling, a constant that does NOT fit `int` ──────
BEZEL_TEST(sema_string_repeat_count_gate_7132,
           reject_static_out_of_range_constant) {
    auto r = runAot(prog(
        "    Console.WriteLine(string.Repeat('x', 2147483648L).Length);\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(notConvertible(r));
    BEZEL_EXPECT_TRUE(constantRangeReason(r));
}

// ── REJECT — the static's NON-integer-like count keeps ITS OWN wording ───
// The static site rejects a `double` count with "requires a `char` (int)
// count" (stdlib §9.5/§9.6). That wording is UNCHANGED by this fix, and the
// §5.1 gate must not fire on the same argument as well: one mistake, one
// diagnostic.
BEZEL_TEST(sema_string_repeat_count_gate_7132,
           reject_static_double_count_keeps_its_own_wording) {
    auto r = runAot(prog(
        "    var d: double := 3.0;\n"
        "    Console.WriteLine(string.Repeat('x', d).Length);\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(requiresCharCount(r));
    BEZEL_EXPECT_FALSE(notConvertible(r));
}

// ── ACCEPT-CONTROLS — the counts §5.1 DOES admit ─────────────────────────
// Every control asserts a VALUE, so a control that stopped lowering would
// fail here rather than silently returning something else.

// §5.1's constant-narrowing clause: a `long` CONSTANT whose value fits
// `int`'s range converts by VALUE, across signedness. This is the row the
// clause exists for — the gate must NOT turn it into a rejection.
BEZEL_TEST(sema_string_repeat_count_gate_7132,
           accept_static_string_unit_const_long_count) {
    const std::string src = prog(
        "    const N: long := 3L;\n"
        "    Console.WriteLine(String.Repeat(\"x\", N).Length);\n"
        "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "3");
}

// The same, for the STATIC spelling.
BEZEL_TEST(sema_string_repeat_count_gate_7132,
           accept_static_const_long_count) {
    const std::string src = prog(
        "    const N: long := 3L;\n"
        "    Console.WriteLine(string.Repeat('x', N).Length);\n"
        "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "3");
}

// The plain `int` count — the declared parameter type.
BEZEL_TEST(sema_string_repeat_count_gate_7132, accept_static_string_unit_int_count) {
    const std::string src = prog(
        "    var n: int := 3;\n"
        "    Console.WriteLine(String.Repeat(\"x\", n).Length);\n"
        "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "3");
}

BEZEL_TEST(sema_string_repeat_count_gate_7132, accept_static_int_count) {
    const std::string src = prog(
        "    var n: int := 3;\n"
        "    Console.WriteLine(string.Repeat('x', n).Length);\n"
        "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "3");
}

// `byte` and `short` widen to `int` under §5.1's table.
BEZEL_TEST(sema_string_repeat_count_gate_7132, accept_static_string_unit_byte_count) {
    const std::string src = prog(
        "    var b: byte := 3;\n"
        "    Console.WriteLine(String.Repeat(\"x\", b).Length);\n"
        "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "3");
}

BEZEL_TEST(sema_string_repeat_count_gate_7132, accept_static_string_unit_short_count) {
    const std::string src = prog(
        "    var b: short := 3;\n"
        "    Console.WriteLine(String.Repeat(\"x\", b).Length);\n"
        "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "3");
}

BEZEL_TEST(sema_string_repeat_count_gate_7132, accept_static_byte_count) {
    const std::string src = prog(
        "    var b: byte := 3;\n"
        "    Console.WriteLine(string.Repeat('x', b).Length);\n"
        "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "3");
}

// A literal count — §4.2's target-typed integer constant, admitted at an
// `int` target.
BEZEL_TEST(sema_string_repeat_count_gate_7132, accept_static_string_unit_literal_count) {
    const std::string src = prog(
        "    Console.WriteLine(String.Repeat(\"x\", 3).Length);\n"
        "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "3");
}

BEZEL_TEST(sema_string_repeat_count_gate_7132, accept_static_literal_count) {
    const std::string src = prog(
        "    Console.WriteLine(string.Repeat('x', 3).Length);\n"
        "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "3");
}

// ── ACCEPT-CONTROL — the sibling PAD/SPAN gates are UNCHANGED ────────────
// §9.6's `PadLeft`/`AsSpan` declare an `int` parameter too, and they were
// already gated. A fix that moved the gate must not have relaxed them.
BEZEL_TEST(sema_string_repeat_count_gate_7132,
           accept_sibling_padleft_still_rejects_uint) {
    auto r = runAot(prog(
        "    var b: uint := 3;\n"
        "    Console.WriteLine(\"x\".PadLeft(b).Length);\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(signedUnsignedReason(r));
}

BEZEL_TEST(sema_string_repeat_count_gate_7132,
           accept_sibling_aspan_still_rejects_uint) {
    auto r = runAot(prog(
        "    var b: uint := 3;\n"
        "    Console.WriteLine(\"x\".AsSpan(b).Length);\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(signedUnsignedReason(r));
}
