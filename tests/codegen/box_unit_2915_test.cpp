// fix #2915 — a boxed unit `var o: Object := ();` was the NULL reference
// (spec §4.7 *The unit type*, §8.6 *Boxing of the unit value*, §8.1, §8.5,
// §8.3 — v0.240). The last item of the §8.6 "unboxed value kinds" family, after
// #2793 (decimal) and #2804 (non-owning inline arrays).
//
// WHAT THE DOCUMENTS REQUIRE.  §4.7:
//
//   "The unit type is a value type and can in principle be boxed via the
//    `Object` derivation chain ([§8.6]) — `var o: Object := ();` is
//    grammatically valid and produces a boxed `()`."
//
// §8.1: "Every type derives from `Object`. … records, enums, primitives, inline
// arrays, and the unit type derive via boxing ([§8.6]) when used in
// `Object`-typed positions."
//
// §8.6, *Boxing of the unit value*: "The unit type `()` has exactly one value,
// so all boxings of `()` may share a single heap object. Implementations are
// permitted (but not required) to intern boxed `()` … The only guarantee is the
// standard one: a boxed `()` value, when unboxed, yields `()`."  BOTH permitted
// readings require a real heap object; the null reference is neither.
//
// §8.5 fixes the rendering: "For inline arrays … and for the unit type: the
// type's name — … `"()"` for unit".
//
// MEASURED on f069645 — `var o: Object := (); Console.WriteLine(o.ToString());
// if o is Object then return 0; end; return 1;` printed **"null"** and exited
// **1**, behind a single `internal: lowerLocalDecl's initializer expression
// failed to lower to a value … — #2434` Warning.
//
// THE CAUSE was not a missing name in a list.  `isBoxableValueType` did end with
// an explicit `return false; // unit: deferred`, but flipping that alone fixes
// nothing: the unit type lowers to LLVM `void`, so a unit-typed expression has
// **no lowered llvm::Value at all** — `lowerExpr` returns null — and every
// boxing site's `if (v)` guard therefore skipped the box before any predicate
// was consulted.  Four positions, four different silences:
//
//   local decl   `var o: Object := ();`  -> slot zero-initialized (#2434 note)
//   assignment   `p := ();`              -> `if (!rhs) return false;` — the
//                                           store did not happen AT ALL and
//                                           NOTHING was reported (measured: `p`
//                                           still held its previous "seed")
//   argument     `Take(())`              -> `internal: lowerExpr returned null
//                                           for call argument … — #1386`, a
//                                           hard compile FAILURE
//   return       `return ();` : Object   -> `#2491` note, caller got null
//
// THE FIX routes all four through the ONE funnel (`boxForTarget`), which now
// answers the unit source before its own `!v` bail; the box itself
// (`boxUnitValue`) is byte-identical in layout to every other box, so §28.9's
// release machinery needs no unit special case — which is also why
// `isBoxableValueType` must admit the unit type: that predicate is what makes
// the release placement see the slot as holding a box (without it the wrapper
// leaks).
//
// A READING THE SPEC DOES NOT STATE, recorded here and in the commit: §8.3's
// table has NO unit row. The unit type has exactly one value, so the Primitive
// row's "Value comparison" can only mean "the other operand is also a boxed
// unit", and "Value-based" hashing of a one-valued type can only be a constant.
// §8.3's invariant ("if a.Equals(b) then a.GetHashCode() = b.GetHashCode()")
// then holds by construction. §8.6 permits but does not require interning; this
// implementation mints a FRESH box per boxing, which §8.4 explicitly leaves
// unspecified for `ReferenceEquals` either way.
//
// ⚠ ON THE ASSERTED VALUES.  The pre-fix behaviour was null / false / 0, so no
// expectation below is allowed to be 0, false or the empty string where the
// property under test could satisfy it vacuously: the hash is asserted equal to
// the non-degenerate constant the thunk returns (0x756E6974 = 1970170228, NOT
// 0 — #2804 recorded that three distinct boxes all "agreeing" at hash 0 is
// exactly how a hash test passes for the wrong reason), the rendering is
// asserted to be the exact string "()", and every type test is paired with a
// negative control that must NOT match.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " end Main; end C;";
}

}  // namespace

// =====================================================================
// #2915 — the issue's own repro, at each of §8.6's four boxing positions.
// =====================================================================

BEZEL_TEST(codegen_box_unit_2915, unit_boxes_at_a_local_decl_spec_4_7_8_1) {
    // Pre-fix: printed "null" and exited 1.
    JitResult r = runAot(prog(
        "var o: Object := (); "
        "Console.WriteLine(o.ToString()); "
        "if o is Object then return 41; end; "
        "return 1;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
    BEZEL_EXPECT_EQ(r.stdoutText.substr(0, 2), std::string("()"));
}

BEZEL_TEST(codegen_box_unit_2915, unit_boxes_on_assignment_spec_4_7_8_6) {
    // Pre-fix: the store did not happen and NOTHING was reported — `p` still
    // held "seed", so this printed "seed".
    JitResult r = runAot(prog(
        "var p: Object := \"seed\"; "
        "p := (); "
        "Console.WriteLine(p.ToString()); "
        "if p is Object then return 42; end; "
        "return 1;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
    BEZEL_EXPECT_EQ(r.stdoutText.substr(0, 2), std::string("()"));
    BEZEL_EXPECT_TRUE(r.stdoutText.find("seed") == std::string::npos);
}

BEZEL_TEST(codegen_box_unit_2915, unit_boxes_as_an_object_argument_spec_4_7) {
    // Pre-fix: the compile FAILED — "internal: lowerExpr returned null for call
    // argument in buildOrderedArgs — #1386".
    JitResult r = runAot(
        "module M; class C "
        "  public static method Take(o: Object): int "
        "    Console.WriteLine(o.ToString()); "
        "    if o is Object then return 43; end; "
        "    return 1; "
        "  end Take; "
        "  public static method Main(): int return Take(()); end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 43);
    BEZEL_EXPECT_EQ(r.stdoutText.substr(0, 2), std::string("()"));
}

BEZEL_TEST(codegen_box_unit_2915, unit_boxes_through_an_object_return_spec_4_7) {
    // Pre-fix: `#2491` note; the caller received null.
    JitResult r = runAot(
        "module M; class C "
        "  public static method Give(): Object return (); end Give; "
        "  public static method Main(): int "
        "    var q: Object := Give(); "
        "    Console.WriteLine(q.ToString()); "
        "    if q is Object then return 44; end; "
        "    return 1; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);
    BEZEL_EXPECT_EQ(r.stdoutText.substr(0, 2), std::string("()"));
}

BEZEL_TEST(codegen_box_unit_2915, unit_boxes_from_a_named_unit_local_spec_4_7) {
    // The one shape that already half-worked pre-fix (a named `()`-typed local
    // lowers to the #1619 one-byte placeholder, so `v` was non-null) — kept as a
    // control that the fix did not disturb it.
    JitResult r = runAot(prog(
        "var u: () := (); "
        "var o: Object := u; "
        "Console.WriteLine(o.ToString()); "
        "if o is Object then return 45; end; "
        "return 1;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 45);
    BEZEL_EXPECT_EQ(r.stdoutText.substr(0, 2), std::string("()"));
}

// =====================================================================
// #2915 — §8.5's rendering is a FIXED STRING, not merely "not null".
// =====================================================================

BEZEL_TEST(codegen_box_unit_2915, boxed_unit_tostring_is_the_type_name_spec_8_5) {
    // §8.5: "for … the unit type: the type's name — … `"()"` for unit".
    // Pre-fix this was "null". Asserting the exact text matters: "not null"
    // would also pass on "", on "0", and on "System.Object".
    BEZEL_EXPECT_EQ(runAotOut(prog("var o: Object := (); "
                                   "Console.WriteLine(o.ToString()); "
                                   "return 0;")),
                    std::string("()"));
}

BEZEL_TEST(codegen_box_unit_2915,
           boxed_unit_renders_through_interpolation_spec_8_5_18_5) {
    // §8.5's opening: "`ToString` is invoked implicitly inside string
    // interpolation ([§18.5]) via virtual dispatch." Through the box, that is
    // the same thunk — pre-fix the hole rendered "null".
    BEZEL_EXPECT_EQ(runAotOut(prog("var o: Object := (); "
                                   "Console.WriteLine($\"<{o}>\"); "
                                   "return 0;")),
                    std::string("<()>"));
}

// =====================================================================
// #2915 — §8.1: the boxed unit IS an Object, and is not anything else.
// Every positive is paired with a negative that must NOT match, so a
// type test that answered `true` for everything could not pass.
// =====================================================================

BEZEL_TEST(codegen_box_unit_2915, boxed_unit_type_tests_precisely_spec_8_1) {
    JitResult r = runAot(prog(
        "var acc := 0; "
        "var o: Object := (); "
        "if o is Object then acc := acc + 1; end; "
        "if o is int then acc := acc + 2; end; "         // must NOT fire
        "if o is string then acc := acc + 4; end; "      // must NOT fire
        "var i: Object := 7; "
        "if i is int then acc := acc + 8; end; "         // control: still works
        "if i is Object then acc := acc + 16; end; "     // control
        "return acc;"));
    BEZEL_EXPECT_TRUE(r.ran);
    // 1 + 8 + 16 = 25. Pre-fix the unit half was 0 and only the int controls
    // contributed (24), so the expected value is not reachable by accident.
    BEZEL_EXPECT_EQ(r.exitCode, 25);
}

// =====================================================================
// #2915 — §8.3 Equals / GetHashCode. The spec has no unit row; the
// reading implemented is stated in the header and the commit message.
// =====================================================================

// ⚠ THIS ONE PASSES WITHOUT THE FIX — measured, not assumed (revert `src/`,
// rebuild, re-run: 2 of the 13 tests in this file still pass and this is one of
// them). Pre-fix `a` and `b` are BOTH the null reference, so "two boxed units
// are equal and hash alike" is satisfied vacuously by two nulls. It is kept
// because it is the positive statement of §8.3's rule, and it is not left
// unguarded: `boxed_unit_hash_is_not_degenerate_spec_8_3` below pins the actual
// hash CONSTANT and does fail without the fix. Read the two as one assertion.
BEZEL_TEST(codegen_box_unit_2915, boxed_unit_equality_and_hash_spec_8_3) {
    JitResult r = runAot(prog(
        "var acc := 0; "
        "var a: Object := (); "
        "var b: Object := (); "
        "var i: Object := 7; "
        "var s: Object := \"x\"; "
        "if a.Equals(b) then acc := acc + 1; end; "               // yes
        "if a.GetHashCode() = b.GetHashCode() then acc := acc + 2; end; "  // yes
        "if a.Equals(i) then acc := acc + 4; end; "               // must NOT
        "if i.Equals(a) then acc := acc + 8; end; "               // must NOT
        "if a.Equals(s) then acc := acc + 16; end; "              // must NOT
        "if a.GetHashCode() = i.GetHashCode() then acc := acc + 32; end; "  // no
        "return acc;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

BEZEL_TEST(codegen_box_unit_2915, boxed_unit_hash_is_not_degenerate_spec_8_3) {
    // ⚠ The guard bit #2804 asked for by name. Pre-fix every dropped/unboxed
    // path reported hash 0, so an `a.GetHashCode() = b.GetHashCode()` test alone
    // passes vacuously. Pin the actual constant: 0x756E6974 = 1970170228.
    BEZEL_EXPECT_EQ(runAotOut(prog("var o: Object := (); "
                                   "Console.WriteLine(o.GetHashCode()); "
                                   "return 0;")),
                    std::string("1970170228"));
}

// =====================================================================
// #2915 — §28.9: the box is runtime-managed and must not leak.
// This is the reason isBoxableValueType has to admit the unit type: the
// §28.9 release placement is gated on it, so without that leg every
// boxed `()` would leak its 24-byte wrapper.
// =====================================================================

BEZEL_TEST(codegen_box_unit_2915, boxed_unit_loop_is_leak_clean_spec_28_9) {
    // runAotChecked's LSan leg flips `ran` on a leak / double-free / UAF, and
    // the exit code independently proves the boxing happened (pre-fix `acc`
    // stayed 0 and the program returned 1).
    JitResult r = runAotChecked(prog(
        "var i := 0; var acc := 0; "
        "while i < 20000 do "
        "  var o: Object := (); "
        "  if o is Object then acc := acc + 1; end; "
        "  i := i + 1; "
        "end; "
        "if acc = 20000 then return 0; end; "
        "return 1;"));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// =====================================================================
// #2915 — CONTROLS: the neighbouring unit shapes the fix must not break.
// =====================================================================

BEZEL_TEST(codegen_box_unit_2915, plain_unit_local_still_works_spec_4_7) {
    // `var u: () := ();` never boxes (the target is not Object) and must stay a
    // clean, diagnostic-free compile — the #1619 one-byte placeholder slot.
    // Pre-fix this emitted a spurious `internal: … — #2434` note, which the fix
    // also removes: a unit value legitimately has no lowered value.
    JitResult r = runAot(prog("var u: () := (); return 46;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 46);
    for (const auto& d : r.diagnostics.diagnostics())
        BEZEL_EXPECT_TRUE(d.message.find("#2434") == std::string::npos);
}

BEZEL_TEST(codegen_box_unit_2915, other_boxable_kinds_unaffected_spec_8_6) {
    // The over-rejection / over-acceptance sweep: admitting the unit type into
    // isBoxableValueType must not disturb any other kind's boxing, rendering or
    // identity. Each addend is distinct, so one broken kind changes the sum.
    // The total exceeds 255, so it goes to stdout (runAotOut) rather than the
    // 8-bit exit status.
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; "
            "record Pt(X: int, Y: int); "
            "class C public static method Main(): int "
            "  var acc := 0; "
            "  var a: Object := 7;        if a is int then acc := acc + 1; end; "
            "  var b: Object := true;     if b is bool then acc := acc + 2; end; "
            "  var c: Object := 2.5;      if c is double then acc := acc + 4; end; "
            "  var d: Object := 'z';      if d is char then acc := acc + 8; end; "
            "  var e: Object := \"s\";      if e is string then acc := acc + 16; end; "
            "  var f: Object := 1.5m;     if f is decimal then acc := acc + 32; end; "
            "  var g: Object := new Pt(1, 2); if g is Pt then acc := acc + 64; end; "
            "  var ia: [3]int := [1, 2, 3]; "
            "  var h: Object := ia;       if h is [3]int then acc := acc + 128; end; "
            "  var u: Object := ();       if u is Object then acc := acc + 256; end; "
            "  if u is int then acc := acc + 512; end; "   // must NOT fire
            "  if a is Object then acc := acc + 1024; end; "
            "  Console.WriteLine(acc); "
            "  return 0; "
            "end Main; end C;"),
        // 1+2+4+8+16+32+64+128+256+1024 = 1535 (bit 512 deliberately absent).
        std::string("1535"));
}
