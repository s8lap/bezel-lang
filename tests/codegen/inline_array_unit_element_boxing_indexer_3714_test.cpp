// fix #3714 (§3.2 *Reference vs. value types*, §8.6 *Boxing*, §15.1 *Inline
// arrays* — v0.294): the two `[N]()` surfaces #3528's own text flagged as
// unvalidated ("§8.6 boxing of an inline array") and left broken.
//
// ── (a) BOXING — `descTypeArgConcrete` (LlvmBackend_Setup.cpp, a free
// function used by `Emitter::inlineArrayBoxable`) had arms for
// `Kind::InlineArray` and `Kind::Named` and otherwise fell through to
// `kind == Kind::Primitive` — no `Kind::Unit` arm. A unit-typed element
// therefore read as "not concrete", `inlineArrayBoxable` refused `[3]()`, and
// `var o: Object := a;` did not go through the normal box-vtable-minting
// path. Any later virtual dispatch on the resulting (invalidly-vtabled) box
// — `o.ToString()` — then failed to lower and fell into the pre-existing
// `Severity::Warning`-class #3079 "argument … failed to lower" degrade
// guard, which drops the WHOLE STATEMENT (stdlib §2.1/§21.9's own text:
// "not even the line terminator").
//
// MEASURED pre-fix (this exact program, `bezelc y8.bzl -o y8; ./y8`):
//   y8.bzl:7:23: internal: the argument of this `Console.WriteLine` failed
//     to lower to a value, so the whole statement emits nothing — not even
//     the line terminator (stdlib §2.1/§21.9) — #3079
//   $ ./y8
//   [3]()                     <- ONE line; o.ToString()'s line is MISSING
// exit 0 either way (Warning-severity; #3079 does not fail the compile).
//
// THE FIX: `descTypeArgConcrete` grows a `Kind::Unit` arm alongside
// `Kind::Primitive` — the unit type has no generic-argument ambiguity (it is
// a single concrete type, exactly like a primitive tag), matching the
// issue's own traced root cause.
//
// ── (b) INDEXER-SETTER RHS — the dedicated indexer-setter arm in
// `AssignmentStmt`'s `:=` handling (LlvmBackend_Stmts.cpp; NOT
// LlvmBackend_Assign.cpp, which the issue guessed but a prior lane's
// investigation ruled out) could not tell a genuinely FAILED RHS lowering
// from a legitimately UNIT-TYPED one — both lower to `nullptr` by the #3421
// convention (there is no runtime value for `()`). So `a[1] := ();` on a
// `[3]()` array drew the #3025 "indexer-setter RHS … failed to lower"
// degrade guard and the store never happened, even though `()` is a
// perfectly well-typed RHS.
//
// MEASURED pre-fix (this exact program, `bezelc y11.bzl -o y11; ./y11`):
//   y11.bzl:5:13: internal: indexer-setter RHS expression failed to lower
//     (degraded — target retains its previous value) — #3025
//   $ ./y11
//   3
//   ()
// (This repro's OWN stdout happens to be unchanged either way — Unit has
// exactly one value, so "the store didn't happen" and "the store happened"
// read back identically. The DEFECT is that the assignment statement itself
// is a no-op and, in general, degrades a legal write; the #3025 line is the
// only side of this that can actually flip, which is why the companion lit
// test `tests/lit/inline_array_unit_element_boxing_indexer_no_warn_3714.bzl`
// — not this file — carries the falsifiable half of this fix's proof.)
//
// THE FIX mirrors fix #3712's `lowerCollectionLiteralElementOrUnitNull`
// decision (LlvmBackend_impl.hpp / LlvmBackend_Setup.cpp): keyed on the
// RHS's OWN sema type being `Kind::Unit` (never on the lowered value being
// null), and ONLY when the indexed target itself is a plain `[N]T` inline
// array — an indexer PROPERTY or a heap collection whose RHS genuinely
// failed to lower still degrades via #3025 exactly as before.
//
// FAIL-WITHOUT / PASS-WITH (src/ restored to this fix's parent via a scratch
// checkout, this test file kept, then `bezel_tests_codegen` relinked —
// LANE_RULES §3, NOT a `bezelc`-only rebuild):
//   T1 (boxing, repro a)      0 passed, 1 failed (stdoutText missing line 2)
//   T2 (indexer-set, repro b) 1 passed, 0 failed (a CONTROL — see above; the
//                             falsifiable half of (b) lives in the lit test)
// PASS-WITH: 2 passed, 0 failed (bezel_tests_codegen,
//   filter=codegen_inline_array_unit_element_boxing_indexer_3714).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// ── T1 — repro (a): boxing a `[3]()` value into `Object` and dispatching a
// virtual member (`ToString()`) on the box.
BEZEL_TEST(codegen_inline_array_unit_element_boxing_indexer_3714,
           unit_element_inline_array_boxes_and_dispatches_tostring_spec_8_6) {
    auto r = runAotChecked(
        "module M3714A; "
        "class C "
        "  public static method Main(): int "
        "    var a: [3]() := [(), (), ()]; "
        "    Console.WriteLine(a.ToString()); "
        "    var o: Object := a; "
        "    Console.WriteLine(o.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // §8.5's inline-array ToString row ("the type's name — `[3]int`-style")
    // applies identically through the box: both the direct and boxed calls
    // render "[3]()".
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("[3]()\n[3]()\n"));
}

// ── T2 — repro (b): the indexer-setter write itself. A CONTROL (see the
// file header) that pins the surrounding behavior stays correct post-fix —
// Length unaffected, the written-back element still reads as `()`, and the
// array's own ToString is unaffected. Passes on both sides of the fix by
// Unit's own single-valuedness; the falsifiable pin for (b) is the sibling
// lit test's #3025 CHECK-NOT.
BEZEL_TEST(codegen_inline_array_unit_element_boxing_indexer_3714,
           unit_element_indexer_set_control_spec_15_1) {
    auto r = runAotChecked(
        "module M3714B; "
        "class C "
        "  public static method Main(): int "
        "    var a: [3]() := [(), (), ()]; "
        "    a[1] := (); "
        "    Console.WriteLine(a.Length); "
        "    Console.WriteLine(a[1].ToString()); "
        "    Console.WriteLine(a.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n()\n[3]()\n"));
}
