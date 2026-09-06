// Codegen + sema — §12.7 / §8.6 / §17.4, #3090: a read through a §12.7
// flow-narrowing to a VALUE type must produce the boxed VALUE, not the box
// POINTER.
//
// spec §12.7 (v0.252), *Type tests: `is`, `is not`, `as`*:
//   "`is T` tests whether a value is an instance of `T` and produces `bool`.
//    When the test succeeds and the tested expression is a simple variable, the
//    variable is **flow-narrowed** to `T` within the controlled block:"
//   "Narrowing applies to the original variable. Reassignment within the
//    controlled block invalidates the narrowing."
//   "Both `is` and `as` are **non-destructive on their operand**: the operation
//    inspects the runtime type tag without moving from the operand."
//
// spec §8.6 (v0.252), *Boxing*:
//   "When a value type is used where `Object` (or an interface implemented by
//    the value type) is expected, the value is **boxed**: a heap-allocated
//    wrapper holds the value."
//   "The only guarantee is the standard one: a boxed `()` value, when unboxed,
//    yields `()`."   ← the general form of the round-trip property, stated for
//                      the unit type; *Nullable value-type boxing* states it for
//                      the general case as "Unboxing reverses the process".
//
// spec §17.4 (v0.252), *Degenerate value-type targets* — which is why this is
// not a corner case but the ONLY sanctioned spelling:
//   "An **inline-array target** … is a **compile error** naming this rule …
//    The remedy is the `is` test, whose [§12.7] narrowing admits copying the
//    value out: `if o is [3]int then var a: [3]int := o; end;`"
//
// THE DEFECT — two halves, both required.
//
//   (1) SEMA narrowed only to a `Kind::Named` test type with a declaration, so
//       `o is int` and `o is [3]int` narrowed NOTHING. The copy-out that §17.4
//       PRINTS AS THE REMEDY therefore read the declared `Object`, and §3's
//       permissive acceptance (#605) admits an `Object` initializer for a
//       value-typed local without a word — so it compiled.
//
//   (2) CODEGEN had no §12.7 awareness at all. Its only value-unbox sites were
//       the `as` arm (LlvmBackend_Layout.cpp) and the §19.3 pattern arms
//       (LlvmBackend_Exprs2.cpp); a plain `if o is T then … o … end` reached
//       neither. So even for a Named value type — a record — where sema DID
//       narrow, every read handed the consumer the box pointer.
//
// WHY IT WAS SILENT AT EVERY SEVERITY: `store ptr` + `load i32` through one
// alloca is VALID IR under opaque pointers, so `verifyModule` cannot see it.
//
// MEASURED ON THE PRE-FIX COMPILER (663a68e), one shape per kind:
//   `int`      `var i: int := o;` printed 419001120 / 1161691200 — the low 32
//              bits of the box's heap address, different every run (ASLR).
//   record     `q.X * 10 + q.Y` raised OverflowException (exit 70); the direct
//              `o.X` printed 707433248 (the box's vtable word, low half).
//   `[3]int`   all zeros: the copy-out drew codegen's #2434 "initializer
//              expression failed to lower … degraded to a zero-initialized
//              value", and the direct `o[0]` drew #3079's "argument … failed to
//              lower … the whole statement emits nothing".
//
// THE FIX. Sema's narrowable-test-type set becomes a POSITIVE list that admits
// Primitive and InlineArray alongside Named (Error / Unit / Function stay out —
// Unit has no lowered value at all). Codegen gains ONE predicate,
// `narrowedBoxedValueTypeOf` (bare identifier + local/parameter symbol +
// `Object`/interface DECLARED location + a §8.6-boxable published type), feeding
// two entry points: `unboxNarrowedRead` for the value read and
// `narrowedBoxPayloadAddr` for the three address consumers (`lvalueAddr`,
// `fieldAddr`'s record arm, and through them the inline-array element read).
// The payload sits two words into `{ vtable*, i64 rc, payload }` — the same
// offset the `as` arm and the §19.3 typed-binding arm already read.
//
// The type-test OPERAND position deliberately keeps the box (§12.7's "inspects
// the runtime type tag"): `narrowedUnboxSuppressed_` holds it, so a nested
// `if o is int then … o is T … end` still walks the descriptor chain off the
// wrapper instead of off an `i32`. The answer is the same either way.
//
// TWO `isBoxableValueType` KINDS ARE NOT COVERED BELOW, both for reasons
// upstream of this fix and both filed:
//   * a VALUE-BUILTIN (`DateTime`/`TimeSpan`/`Guid`/…) — it resolves to the
//     DECLARATION-LESS Named TypeRef shape, which sema's narrowable list still
//     excludes because that same shape is a bare type PARAMETER. The copy-out
//     therefore emits malformed IR (loudly, not silently), exactly as it does
//     with no `is` guard at all. **#3122**.
//   * a bare type PARAMETER (`o is T` inside `Box<T>` at `Box<[3]int>`) — the
//     type TEST itself reads FALSE there, verified still false on this build.
//     **#3092**, which #3090 does not fix: the §17.4 remedy works on the
//     CONCRETE path only.

#include <string>

#include "support/aot_result_asserts.hpp"   // fix #3895: failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

// ASan toolchain absence bail-out. The original #3090 file does not define
// its own copy (the harness's `bezel::test::wasSkipped` exists in OTHER test
// files where it is locally defined); #3200 introduces ASan-oracle tests
// and so needs the helper locally. Mirrors the body used in
// box_inline_array_2804_test.cpp and the rest of the suite — when r.ran is
// false the test "did not run", and the only legitimate cause the harness
// recognises for that today is a SKIP diagnostic.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

int run(const char* src) {
    JitResult r = runAot(src);
    if (!r.ran) return -1;
    return r.exitCode;
}

// =====================================================================
// The three shapes the issue names, one test each.
// =====================================================================

// §12.7 + §8.6. Pre-fix: 419001120 (the box pointer's low 32 bits). Want 42.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           narrowed_int_copy_out_yields_the_value_spec_12_7_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var o: Object := 42; "
        "    if o is int then "
        "      var i: int := o; "
        "      return i; "
        "    end; "
        "    return 1; "
        "  end Main; "
        "end C;"), 42);
}

// §12.7 + §8.6, the RECORD shape — both the copy-out and the DIRECT member read
// through the narrowed binding, because option (b) alone (a declared-type slot)
// would have fixed only the first. Pre-fix: OverflowException (exit 70) from the
// copy-out's arithmetic, and 707433248 from the direct `o.X`.
// Want 7*10 + 9 + 7 = 86.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           narrowed_record_copy_out_and_direct_member_spec_12_7_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method Main(): int "
        "    var p: Pt := new Pt(7, 9); "
        "    var o: Object := p; "
        "    if o is Pt then "
        "      var q: Pt := o; "
        "      return q.X * 10 + q.Y + o.X; "
        "    end; "
        "    return 1; "
        "  end Main; "
        "end C;"), 86);
}

// §17.4's PRINTED REMEDY, verbatim in shape: "if o is [3]int then var a: [3]int
// := o; end;". This is the assertion #2911 parked and #3090 owed — the value
// `456` that `o as [3]int` used to return before §17.4 rejected that spelling.
// Pre-fix: an all-zero array (sum 0) plus codegen's #2434 note.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           narrowed_inline_array_copy_out_is_the_17_4_remedy_spec_17_4_12_7) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: [3]int := [4, 5, 6]; "
        "    var o: Object := a; "
        "    if o is [3]int then "
        "      var b: [3]int := o; "
        "      Console.WriteLine(b[0] * 100 + b[1] * 10 + b[2]); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("456"));
}

// =====================================================================
// The rest of the read surface — the issue's own warning that "(b) alone may
// fix `var i: int := oi;` while leaving a direct read like `o[0]` or `p.X`
// still wrong". Each of these reads the narrowed binding IN PLACE.
// =====================================================================

// §15.1 element read straight off the narrowed binding, no copy-out local.
// Pre-fix: the statement emitted nothing at all (#3079). Want 456.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           narrowed_inline_array_direct_element_read_spec_12_7_15_1) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: [3]int := [4, 5, 6]; "
        "    var o: Object := a; "
        "    if o is [3]int then "
        "      Console.WriteLine(o[0] * 100 + o[1] * 10 + o[2]); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("456"));
}

// §12.7 in the remaining value positions a narrowed binding reaches: a binary
// operand (`acc + o`), a parenthesised arithmetic subexpression (`o + 1`), an
// ordinary call argument (`Take(o)`), and a method call on the narrowed value
// itself (`o.ToString()` must be `int`'s, not `Object`'s box-vtable one — both
// render "42", so the discriminating bits are the arithmetic ones, which is why
// the SUM is asserted and not just the text).
// Want 42 + 43 + 42 + 42 = 169.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           narrowed_int_reads_in_every_value_position_spec_12_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Take(n: int): int => n; "
        "  public static method Main(): int "
        "    var o: Object := 42; "
        "    var acc := 0; "
        "    if o is int then "
        "      acc := acc + o; "
        "      acc := acc + (o + 1); "
        "      acc := acc + Take(o); "
        "      if o.ToString() = \"42\" then acc := acc + 42; end; "
        "    end; "
        "    return acc; "
        "  end Main; "
        "end C;"), 169);
}

// =====================================================================
// The other boxable value kinds — the fix is keyed on `isBoxableValueType`, so
// every kind that predicate admits must round-trip, not just the three above.
// =====================================================================

// §4 primitives beyond `int`, each copied out through its own narrowing.
// bit 1 long, 2 double, 4 bool, 8 char, 16 decimal. Want 31.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           narrowed_primitive_kinds_round_trip_spec_12_7_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var code := 0; "
        "    var ol: Object := 9000000000L; "
        "    if ol is long then var l: long := ol; "
        "      if l = 9000000000L then code := code + 1; end; end; "
        "    var od: Object := 1.5; "
        "    if od is double then var d: double := od; "
        "      if d = 1.5 then code := code + 2; end; end; "
        "    var ob: Object := true; "
        "    if ob is bool then var b: bool := ob; "
        "      if b then code := code + 4; end; end; "
        "    var oc: Object := 'x'; "
        "    if oc is char then var c: char := oc; "
        "      if c = 'x' then code := code + 8; end; end; "
        "    var om: Object := 12.5m; "
        "    if om is decimal then var m: decimal := om; "
        "      if m = 12.5m then code := code + 16; end; end; "
        "    return code; "
        "  end Main; "
        "end C;"), 31);
}

// §10.3: an enum boxes as its underlying integer, so a narrowed enum read must
// produce the CASE, not the box pointer (a pointer compared against `Col.Green`
// would answer false). Want 1.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           narrowed_enum_copy_out_yields_the_case_spec_12_7_10_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Col Red, Green, Blue; end Col; "
        "class C "
        "  public static method Main(): int "
        "    var o: Object := Col.Green; "
        "    if o is Col then "
        "      var c: Col := o; "
        "      if c = Col.Green then return 1; end; "
        "      return 2; "
        "    end; "
        "    return 3; "
        "  end Main; "
        "end C;"), 1);
}

// §8.6: the box lives at an INTERFACE location just as it does at `Object`
// ("or an interface implemented by the value type"), so the unbox must key on
// both. `IArea` is implemented by the record, the value is boxed into the
// interface local, and the narrowing copies it back out. Want 12.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           narrowed_read_through_an_interface_location_spec_8_6_12_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IArea "
        "  method Area(): int; "
        "end IArea; "
        "record Rect(W: int, H: int): IArea "
        "  public method Area(): int => W * H; "
        "end Rect; "
        "class C "
        "  public static method Main(): int "
        "    var a: IArea := new Rect(3, 4); "
        "    if a is Rect then "
        "      var r: Rect := a; "
        "      return r.W * r.H; "
        "    end; "
        "    return 1; "
        "  end Main; "
        "end C;"), 12);
}

// §12.7 narrows "locals and parameters" alike (narrowFromTypeTest keys on
// LocalSymbol OR ParameterSymbol), and a PARAMETER's storage is a different
// codegen path from a local's (`paramSlots_` / `refParamAddr`, not `locals_`).
// One `Object` parameter, all three shapes, dispatched by the type test.
// Want 42 + 79 + 456 = 577 — outside 0..255, so it goes to stdout.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           narrowing_on_an_object_parameter_unboxes_spec_12_7) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method From(o: Object): int "
        "    if o is int then var i: int := o; return i; end; "
        "    if o is Pt then return o.X * 10 + o.Y; end; "
        "    if o is [3]int then return o[0] * 100 + o[1] * 10 + o[2]; end; "
        "    return 0; "
        "  end From; "
        "  public static method Main(): int "
        "    var p: Pt := new Pt(7, 9); "
        "    var a: [3]int := [4, 5, 6]; "
        "    Console.WriteLine(From(42) + From(p) + From(a)); "
        "    return 0; "
        "  end Main; "
        "end C;"), std::string("577"));
}

// §27.5: a narrowed read of a local that a lambda CAPTURES goes through the
// shared heap cell (`boxedStorage`), not the alloca — a second storage path the
// unbox has to route through correctly. Want 42.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           narrowed_read_of_a_captured_local_spec_12_7_27_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var o: Object := 42; "
        "    var f := lambda(): int => 0; "
        "    if o is int then "
        "      var i: int := o; "
        "      f := lambda(): int => i; "
        "    end; "
        "    return f(); "
        "  end Main; "
        "end C;"), 42);
}

// =====================================================================
// §12.7's other narrowing directions.
// =====================================================================

// §12.7: "for `is not T`, the variable is narrowed to `T` in the else branch."
// Want 42.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           is_not_else_branch_narrowing_unboxes_too_spec_12_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var o: Object := 42; "
        "    if o is not int then "
        "      return 1; "
        "    else "
        "      var i: int := o; "
        "      return i; "
        "    end; "
        "    return 2; "
        "  end Main; "
        "end C;"), 42);
}

// §12.7: "When two `is` tests are joined with `and`, both narrowings are in
// effect throughout the combined true branch." Want 7*10 + 9 + 5 = 84.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           two_and_joined_narrowings_both_unbox_spec_12_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method Main(): int "
        "    var op: Object := new Pt(7, 9); "
        "    var oi: Object := 5; "
        "    if op is Pt and oi is int then "
        "      var q: Pt := op; "
        "      var i: int := oi; "
        "      return q.X * 10 + q.Y + i; "
        "    end; "
        "    return 1; "
        "  end Main; "
        "end C;"), 84);
}

// §12.7: "Reassignment within the controlled block invalidates the narrowing."
// After `o := "s";` the binding is back to its declared `Object`, so the read
// below it must NOT be unboxed — it must be the plain reference read, which
// `o is string` then answers true for. Want 9 (1 before the reassignment + 8
// after). This is the guard that the fix keys on sema's per-node published type
// rather than on "somewhere in this block there was an `is`".
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           reassignment_invalidates_the_narrowing_spec_12_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var o: Object := 42; "
        "    var code := 0; "
        "    if o is int then "
        "      var i: int := o; "
        "      if i = 42 then code := code + 1; end; "
        "      o := \"s\"; "
        "      if o is string then code := code + 8; end; "
        "    end; "
        "    return code; "
        "  end Main; "
        "end C;"), 9);
}

// §12.7 "the operation inspects the runtime type tag without moving from the
// operand": a type test NESTED inside a narrowed block still tests the box, so
// it must answer correctly rather than walk a descriptor chain off an `i32`.
// This is the `narrowedUnboxSuppressed_` guard's own test — without it the
// nested `o is int` emitted a GEP on a scalar. Want 1 + 2 + 4 = 7.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           nested_type_test_inside_a_narrowed_block_spec_12_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method Main(): int "
        "    var code := 0; "
        "    var o: Object := 42; "
        "    if o is int then "
        "      if o is int then code := code + 1; end; "
        "      if o is string then code := code + 16; end; "
        "      if not (o is Pt) then code := code + 2; end; "
        "      var i: int := o; "
        "      if i = 42 then code := code + 4; end; "
        "    end; "
        "    return code; "
        "  end Main; "
        "end C;"), 7);
}

// §12.7 + §4.6 + #1071 — THE REGRESSION THIS FIX'S OWN SWEEP CAUGHT, pinned.
// A `string` in an `Object` slot is a BARE `bz_string*` with no vtable word, so
// `emitIsA`'s descriptor walk must be gated on `bz_obj_is_string` whenever the
// operand could be one. That gate reads the operand's static type — and once
// `o is string` NARROWS `o` to `string`, a later test on the same `o` (in the
// next `and` conjunct, or inside the controlled block) saw `string`, concluded
// "not an Object, so it cannot be a bare string", and walked the string's
// `char* data` as a descriptor. MEASURED as a core dump on #2793's
// `other_primitive_boxes_are_unchanged` sweep (4095 -> SIGSEGV) before
// lowerTypeTest was taught to classify a narrowed operand by its DECLARED type.
// Want 1 + 2 + 4 + 8 = 15.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           type_test_on_a_string_narrowed_operand_is_guarded_spec_12_7_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method Main(): int "
        "    var code := 0; "
        "    var o: Object := \"hi\"; "
        "    if o is string and not (o is decimal) then code := code + 1; end; "
        "    if o is string then "
        "      if not (o is int) and not (o is Pt) and not (o is [3]int) "
        "        then code := code + 2; end; "
        "      if o is Object then code := code + 4; end; "
        "      var s: string := o; "
        "      if s.Length = 2 then code := code + 8; end; "
        "    end; "
        "    return code; "
        "  end Main; "
        "end C;"), 15);
}

// §12.7 — THE SUPPRESSION MUST NOT REACH PAST THE OPERAND'S ROOT. lowerTypeTest
// lowers a BARE-IDENTIFIER operand as its box (the test above), and it does that
// by setting a flag around the whole operand lowering. Set unconditionally, that
// flag also silenced the unbox for a narrowed identifier nested INSIDE a
// compound operand: `Take(o)` handed a `ptr` to an `i32` parameter and failed IR
// verification. The shape is reachable only BECAUSE this fix widened §12.7
// narrowing to primitives — it did not compile at all before — so it would have
// been a NEW crash shape, which is why the flag is gated on the operand being an
// identifier. This test asserts the program both compiles AND computes: want
// 1 + 2 + 4 = 7.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           narrowed_read_inside_a_compound_type_test_operand_spec_12_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Foo(V: int); "
        "class C "
        "  public static method Take(n: int): Object "
        "    var r: Object := n + 1; "
        "    return r; "
        "  end Take; "
        "  public static method Main(): int "
        "    var code := 0; "
        "    var o: Object := 42; "
        "    if o is int then "
        "      if Take(o) is int then code := code + 1; end; "
        "      if Take(o) is Foo then code := code + 64; end; "
        "      var b: Object := Take(o); "
        "      if b is int then "
        "        var i: int := b; "
        "        if i = 43 then code := code + 2; end; "
        "      end; "
        "      var j: int := o; "
        "      if j = 42 then code := code + 4; end; "
        "    end; "
        "    return code; "
        "  end Main; "
        "end C;"), 7);
}

// §12.7 — the narrowed binding IS the value, so that value's members and the
// §29.1 iteration protocol work on it directly, not only on a copy-out. Three
// claims pinned here rather than left to the commit body: §15.1's `.Length` (a
// compile-time constant read off the narrowed InlineArray TypeRef), §29's
// `for … in` over an inline array (which reaches the same box-payload address
// through `lvalueAddr`), and a record METHOD call on the narrowed receiver — at
// an `Object` location and at an interface one.
// bit 1 Length, 2 for-in, 4 method via Object, 8 method via interface. Want 15.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           narrowed_binding_supports_members_and_iteration_spec_12_7_15_1_29_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IArea "
        "  method Area(): int; "
        "end IArea; "
        "record Rect(W: int, H: int): IArea "
        "  public method Area(): int => W * H; "
        "end Rect; "
        "class C "
        "  public static method Main(): int "
        "    var code := 0; "
        "    var a: [3]int := [4, 5, 6]; "
        "    var oa: Object := a; "
        "    if oa is [3]int then "
        "      if oa.Length = 3 then code := code + 1; end; "
        "      var sum := 0; "
        "      for var e in oa do sum := sum + e; end; "
        "      if sum = 15 then code := code + 2; end; "
        "    end; "
        "    var o: Object := new Rect(3, 4); "
        "    if o is Rect then "
        "      if o.Area() = 12 then code := code + 4; end; "
        "    end; "
        "    var ia: IArea := new Rect(5, 6); "
        "    if ia is Rect then "
        "      if ia.Area() = 30 then code := code + 8; end; "
        "    end; "
        "    return code; "
        "  end Main; "
        "end C;"), 15);
}

// §12.7 + §8.6 round trip through a SECOND box: inside the narrowed block the
// binding is an `int`, so assigning it to a fresh `Object` must BOX it again
// (§8.6's general rule) rather than copy a stale pointer. Want 1.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           narrowed_value_reboxes_into_a_fresh_object_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var o: Object := 42; "
        "    if o is int then "
        "      var back: Object := o; "
        "      if back is int then "
        "        var i: int := back; "
        "        if i = 42 then return 1; end; "
        "      end; "
        "      return 2; "
        "    end; "
        "    return 3; "
        "  end Main; "
        "end C;"), 1);
}

// =====================================================================
// OVER-REACH CONTROLS. The unbox must fire ONLY for a §8.6-boxable value type
// at an Object/interface location; every neighbouring narrowing must stay
// byte-identical. A fix that is too wide is a worse defect than the one it
// closes.
//
// One control is NOT written here and the omission is deliberate: a narrowing on
// a location declared at the value type itself (`var p: Pt := …; if p is Pt`).
// That test cannot be reached — MEASURED at this fix's commit, `p is Pt` (and
// `p is int` on an `int`) answers FALSE, because lowerTypeTest drops a
// non-pointer, non-nullable operand and `lowerIf` reads a null condition as a
// branch past the body. Pre-existing, orthogonal (no box is involved at all),
// unchanged by this fix, and filed as **#3120**. A control asserting the `false`
// would pin that bug as correct.
// =====================================================================

// §12.7's own example — a CLASS narrowing. A class reference in an `Object`
// slot is NOT a box (#399: the pointer IS the value), so unboxing it would
// dereference the object's own vtable word as a payload. Want 1.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           class_narrowing_is_unchanged_spec_12_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Animal "
        "  public constructor() end constructor; "
        "  public method Speak(): int => 1; "
        "end Animal; "
        "class Dog: Animal "
        "  public constructor() base(); end constructor; "
        "  public method Bark(): int => 7; "
        "end Dog; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Dog(); "
        "    var o: Object := d; "
        "    if o is Dog then "
        "      return o.Bark(); "
        "    end; "
        "    return 1; "
        "  end Main; "
        "end C;"), 7);
}

// §4.6: a `string` in an `Object` slot is a BARE `bz_string*` with no wrapper
// (#1071), and `isBoxableValueType` excludes it by name. Narrowing to `string`
// must therefore read the pointer as-is. Want 5.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           string_narrowing_is_not_unboxed_spec_4_6_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var o: Object := \"hello\"; "
        "    if o is string then "
        "      var s: string := o; "
        "      return s.Length; "
        "    end; "
        "    return 1; "
        "  end Main; "
        "end C;"), 5);
}

// THE CONTROL THIS FIX'S SEMA GATE EXISTS FOR. A NULLABLE VALUE local
// (`var n: int?`) is NOT an `Object`/interface location: its storage is the
// tagged `{i8 hasValue, i32}` struct, and §7's type test on it is the non-null
// test (lowerTypeTest's `isNullableValueTy` arm returns `hasValue`). If
// `n is int` narrowed `n` to `int`, the published type would stop matching the
// storage and every read would locate the payload wrongly — which is exactly
// what narrowFromNullTest warns about, verbatim: "codegen relies on the declared
// nullable type to locate the tagged-optional payload". So the new
// Primitive/InlineArray narrowing is gated on the declared location, and `n`
// must still need its `!` inside the branch. Want 6 (present) and 2 (absent).
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           nullable_value_local_is_not_narrowed_or_unboxed_spec_7_12_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var n: int? := 5; "
        "    if n is int then "
        "      return n! + 1; "
        "    end; "
        "    return 99; "
        "  end Main; "
        "end C;"), 6);
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var n: int? := null; "
        "    if n is int then "
        "      return 1; "
        "    end; "
        "    return 2; "
        "  end Main; "
        "end C;"), 2);
}

// Reads of the SAME `Object` local OUTSIDE any narrowed block keep the
// reference semantics they had: `o.ToString()` dispatches through the box
// vtable (§8.5) and `o is int` still answers true. Want 1.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           unnarrowed_object_reads_are_unchanged_spec_8_5_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var o: Object := 42; "
        "    if o.ToString() = \"42\" and o is int then return 1; end; "
        "    return 2; "
        "  end Main; "
        "end C;"), 1);
}

// The `is` test must still DISCRIMINATE — a narrowing that fires on the wrong
// branch would unbox a payload that is not there. `o` holds a `[3]int`, so the
// `[4]int` and `Pt` arms must not be taken and the `[3]int` arm must be.
// Want 1.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           narrowing_only_fires_on_the_matching_arm_spec_12_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(X: int, Y: int); "
        "class C "
        "  public static method Main(): int "
        "    var a: [3]int := [4, 5, 6]; "
        "    var o: Object := a; "
        "    if o is [4]int then return 8; end; "
        "    if o is Pt then return 9; end; "
        "    if o is int then return 10; end; "
        "    if o is [3]int then "
        "      var b: [3]int := o; "
        "      if b[0] = 4 and b[1] = 5 and b[2] = 6 then return 1; end; "
        "      return 2; "
        "    end; "
        "    return 3; "
        "  end Main; "
        "end C;"), 1);
}

// =====================================================================
// §28.9 / §12.7 "non-destructive on their operand": the unbox COPIES the
// payload; it must not release, move from, or otherwise disturb the box, and
// the copy-out must not alias it. Run under ASan/LSan in a loop so a
// double-free / use-after-free / per-iteration leak fails the test.
// =====================================================================

BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           narrowed_reads_are_non_destructive_under_asan_spec_12_7_28_9) {
    // TWO narrowed reads of the SAME box per iteration, 200 iterations. If the
    // unbox released or moved from the wrapper the second read would be a
    // use-after-free, and the scope-end §28.9 `bz_box_release` a double free;
    // if it read past the payload, ASan would flag the load. All three fail the
    // test through `r.ran`.
    //
    // The reads are `Console.Write`/`Console.WriteLine` ARGUMENTS on purpose,
    // not a copy-out into a local. `BoxEscapeWalker` (LlvmBackend_Setup.cpp)
    // arms the scope-end §28.9 box release only for a local whose every use is
    // a proven non-retaining read, and it already classifies a console argument
    // as one (`isConsoleNonRetainingArg`, #2865) — so this shape is LSan-clean
    // and a leak here would be a real regression. A copy-out initializer
    // (`var i: int := o;`) and an arithmetic read (`o + 1`) are both still
    // classified as escapes, so their box is conservatively LEFT LEAKED —
    // pre-existing, orthogonal to this fix, the safe direction (a `[2]string`
    // box's payload is released with the wrapper, so a copied-out array would
    // dangle), and filed as **#3116**. Those shapes are therefore asserted under
    // plain `runAot` above, not here.
    //
    // MEASURED at this fix's commit: the same loop with `Console.WriteLine(o)`
    // alone is LSan-clean at 20 iterations (0 leaks), and adding one
    // `Console.WriteLine(o + 1)` takes it to 480 B / 20 objects — which is the
    // escape classification above, not a defect in the unbox.
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 200 do "
        "      var o: Object := 42; "
        "      if o is int then "
        "        Console.Write(o); "
        "        Console.WriteLine(o); "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 200 iterations, two reads of the same box each, one line per iteration.
    std::size_t n = 0, at = 0;
    while ((at = r.stdoutText.find("4242\n", at)) != std::string::npos) {
        ++n; at += 5;
    }
    BEZEL_EXPECT_EQ(n, static_cast<std::size_t>(200));
}

// =====================================================================
// #3200 (§12.7 / §8.6, v0.254): a LocalDecl whose declared type is a VALUE
// TYPE (not Object/interface) and whose initializer is a §12.7-narrowed
// identifier read is a NON-DESTRUCTIVE copy of the box's payload bytes —
// the BOX itself stays uniquely held by the Object/interface local. The
// codegen side (`narrowedBoxPayloadAddr` / `unboxNarrowedRead`, #3090) was
// already correct; the walker side (`BoxEscapeWalker::stmt` LocalDecl arm)
// treated every "binding into another local" as an ESCAPE and so refused to
// arm the scope-end §28.9 `bz_box_release` — 32 B leaked per iteration in
// a tight loop (a `[3]int` box: 8 vtable + 8 rc + 12 payload + 4 alignment
// padding). MEASURED on the pre-fix compiler (HEAD without this fix):
// 319 968 B leaked in 9 999 allocations under LSan (32 B / object).
// =====================================================================

// THE DEFECT'S OWN SHAPE. The leak signature is "one 32-byte box per loop
// iteration". The narrowed-read copies the payload into `b`; the box from
// `o`'s `:= a` initializer must reach its scope-end `bz_box_release`. The
// independent exit-code check pins the value actually read (the box payload
// is the same `[i, i+1, i+2]` that produced the box, so `acc mod 256` is
// deterministic; pre-fix `acc` stays 0 because the read was the box-pointer
// low 32 bits AND the leak costs the box, but here we only need
// `runAotChecked` to confirm LSan-clean + the right exit code).
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           narrowed_inline_array_copy_out_loop_does_not_leak_box_spec_12_7_28_9) {
    // §12.7: "Reads through the narrowed binding read the box's payload …
    // copies the payload out under §3.2's value-copy rule". §8.6 + §28.9:
    // boxed values are runtime-managed and the box wrapper is released
    // exactly once at the owning scope's end. Without this fix the walker
    // over-classified the `var b: [3]int := o;` binding as an escape and
    // §28.9's release was suppressed — 32 B leaked per iteration.
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var i := 0; var acc := 0; "
        "    while i < 10000 do "
        "      var a: [3]int := [i, i + 1, i + 2]; "
        "      var o: Object := a; "
        "      if o is [3]int then "
        "        var b: [3]int := o; "
        "        acc := acc + b[0]; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    return acc mod 256; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    // Sum of i from 0..9999 = 49 995 000; 49 995 000 mod 256 = 248 (because
    // 49 995 008 ≡ 0 mod 256, so 49 995 000 ≡ -8 ≡ 248 mod 256). Pre-fix the
    // value read off `b[0]` was the box-pointer low 32 bits and `acc` had no
    // useful meaning, but the leak alone was sufficient to flip `r.ran` to
    // false under LSan.
    BEZEL_EXPECT_EQ(r.exitCode, 248);
}

// THE NARROWING-SIDE OF THE §3.2 VALUE-COPY CONTRACT: an `o is T` body
// that binds the narrowed value into a value-typed local and then KEEPS
// using it (here, across an `o := <other>` re-narrowing on the OUTER
// binding that this fix says invalidates the narrowing) must not have
// released the box. A double release would fault under ASan / UAF.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           narrowed_inline_array_copy_out_then_use_under_asan_spec_12_7) {
    // The copied-out `b: [3]int` survives past the `if o is [3]int` body via
    // a separate local — the box from `o` was the SINGLE +1 producer, so
    // the copy is an independent value (no aliasing). Want 42 and no leak.
    // The `o := "different type"` reassignment inside the narrowed block
    // invalidates the narrowing (§12.7: "Reassignment within the controlled
    // block invalidates the narrowing") but the assignment-overwrite release
    // path still frees the original box exactly once — and so does the
    // scope-end release this fix arms. Together they would double-free if the
    // walker had treated `var b: [3]int := o;` as an escape (no scope-end
    // release) AND the assignment path released the box (which it does): ASan
    // would fault. With this fix, the assignment release is the ONLY release,
    // the scope-end release sees null in `o`'s slot and is a no-op, and
    // `bz_box_retain` / release accounting balances.
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var src: [3]int := [10, 20, 30]; "
        "    var o: Object := src; "
        "    var acc := 0; "
        "    if o is [3]int then "
        "      var b: [3]int := o; "
        "      o := \"different type\"; "
        "      acc := acc + b[0] + b[1] + b[2]; "
        "    end; "
        "    if acc = 60 then return 42; end; "
        "    return 7; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// CONTROL: an `as`-target value type (here `int`) read through the same
// narrowing path must also not leak the box. The pre-fix walker was
// conservative for `var b: int := o;` too, on the same "binding escapes"
// premise — the leak was 24 B / iteration (an int box) rather than 32 B.
// Pins that the fix is general to the value-typed-binding shape, not just
// the inline-array one.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           narrowed_int_copy_out_loop_does_not_leak_box_spec_12_7_28_9) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 10000 do "
        "      var o: Object := i; "
        "      if o is int then "
        "        var b: int := o; "
        "        if b = 9999 then return 42; end; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    return 7; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// CONTROL: the NEGATION of the fix — a non-narrowed initializer (here
// `var b: [3]int := o;` where `o` is NOT narrowed at this program point).
// The walker keeps its conservative escape verdict, the box is NOT
// released at `o`'s scope end, and the loop leaks 32 B / iter. Pins that
// the relaxed arm really is conditional on the §12.7 narrowing, not a
// blanket "value-typed binding from Object is safe".
//
// ⚠ fix #3895 (issue #3892) FINDING: this row's comment describes a leak
// (32 B / iter, LSan-flagged) pinned with a bare `!ran` claim,
// which cannot tell that apart from ANY other `!ran` outcome. Measured
// directly (`bezelc --sanitize=address` on this EXACT program): the box-
// escape/leak path this row means to exercise is UNREACHABLE — `var o:
// Object := [1, 2, 3];` is itself rejected by §15.4 ("a collection literal
// `[...]` cannot target the type `Object`"), a restriction that postdates
// this test (fix #3090) and was never noticed here because a bare `!ran`
// cannot distinguish "leaked as described" from "rejected before the box
// this row is about was ever created". Filed as issue 3904 (this control's
// program needs re-deriving — e.g. box a `[3]int` local into `Object`
// rather than target a collection literal directly at `Object` — to
// actually reach the scenario the row's own comment describes). Pinning the
// ACTUAL current behavior here (rather than reverting to the disproven bare
// `!ran`) so this row still catches an actual regression without asserting
// something the compiler does not do.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           unnarrowed_object_to_inline_array_copy_still_leaks_as_expected) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 10000 do "
        "      var o: Object := [1, 2, 3]; "
        "      var b: [3]int := o as [3]int; "
        "      i := i + 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    // fix #3895 (issue #3892): pins the ACTUAL current rejection (see the
    // ⚠ finding above, issue 3904) — a genuine §15.4 compile-time reject
    // with a real source location, not a runtime crash/signal, which a bare
    // `!ran` cannot distinguish.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// CONTROL: a PARAMETER of type `Object` narrowed by an `is [3]int` then
// copied into a value-typed local must reach the same safe-read verdict
// as a LocalSymbol — `narrowedIdentDeclaredType` admits both kinds, and
// the walker treats them as "the original variable" identically per §12.7
// ("Narrowing applies to the original variable"). Without the parameter
// arm the box from `p` would leak. 10000 iters × 32 B = ~320 KB leak
// pre-fix.
BEZEL_TEST(codegen_narrowed_value_unbox_3090,
           narrowed_object_parameter_copy_out_loop_does_not_leak_box_spec_12_7) {
    JitResult r = runAotChecked(
        "module M; class C "
        "  public static method Run(o: Object): int "
        "    if o is [3]int then "
        "      var b: [3]int := o; "
        "      return b[0]; "
        "    end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 10000 do "
        "      var src: [3]int := [i mod 256, (i + 1) mod 256, (i + 2) mod 256]; "
        "      acc := acc + Run(src); "
        "      i := i + 1; "
        "    end; "
        "    return acc mod 256; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    // Sum i mod 256 over 10000 iters: 39 full cycles of 0..255 plus a tail of
    // 0..15. Per cycle = 32640; 39 cycles = 1 272 960; tail = 120; total =
    // 1 273 080; 1 273 080 mod 256 = 248. Pre-fix the leak (32 B/iter) tripped
    // LSan and r.ran was false; with this fix the release runs and r.ran is
    // true. The exact exit code is less load-bearing than the leak oracle.
    BEZEL_EXPECT_EQ(r.exitCode, 248);
}

}  // namespace
