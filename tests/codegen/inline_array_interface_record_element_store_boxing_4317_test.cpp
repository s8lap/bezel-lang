// fix #4317 / spec §15.1 *Inline arrays* / §8.6 *Boxing* (v0.298). §8.6
// states, verbatim: "When a value type is used where `Object` (or an
// interface implemented by the value type) is expected, the value is
// **boxed**: a heap-allocated wrapper holds the value." §15.1 constructs an
// inline array's slots from a collection literal or an element-position
// store; nothing in §15.1 exempts an interface-typed slot from §8.6's box
// obligation.
//
// ROOT CAUSE (traced, LlvmBackend_Stmts.cpp). A genuinely CONFORMING record
// (implements the interface, listed in its inheritance clause — the sema
// half #4310 separately hardens) stored into an interface-typed INLINE ARRAY
// element crashed at runtime, even though the identical value works at a
// scalar `IHolder`-typed local and inside a heap `Array<IHolder>`. Layer:
// STORE/INITIALIZATION, not representation or read/dispatch — measured under
// ASan on the issue's own repro: `global-buffer-overflow`, READ of size 8,
// 2 bytes past `.bz.str.data.53` (the "hello" string constant) — i.e. the
// record's raw (unboxed) value bytes, a single 8-byte string fat-pointer
// word for `RecHolder(V: string)`, were stored directly into the box-
// pointer-shaped slot, and a later `.GetV()` dispatch read those bytes as
// `{vtable*, rc, payload}` and walked off the tiny string global. The scalar
// and heap-`Array<T>` call sites already call `boxForTarget` (LlvmBackend_
// Layout.cpp) after lowering the record value; three inline-array STORE call
// sites in `LlvmBackend_Stmts.cpp` did not:
//   1. `outer[i] := new RecHolder(...)` — the IndexExpr arm's plain-array
//      fallback (`lowerStmt`'s `AssignmentStmt` handling), which returns
//      before ever reaching the generic lvalueAddr fallback's OWN
//      `boxForTarget` call a few dozen lines further down the same function.
//   2. `outer := [new RecHolder(...), ...]` — the whole-array collection-
//      literal REASSIGNMENT arm (`fix #736`'s per-element store loop).
//   3. `return [new RecHolder(...), ...]` from a method declared to return
//      `[N]IHolder` — the ReturnStmt collection-literal arm (`fix #781`'s
//      per-element store loop).
// Each is fixed by calling the existing `boxForTarget(value, elementSemaTy,
// sourceExpr)` helper (LlvmBackend_Layout.cpp) before the store — the same
// helper the working scalar/heap-array paths already use. `boxForTarget` is
// a no-op for a CLASS element (already a bare pointer, `isBoxableValueType`
// false) and for any non-Object/interface element type, so every
// already-working shape (class elements, scalar locals, heap `Array<T>`) is
// unaffected — see the `*_class_element_unaffected_control` rows below.
//
// SCOPE, MEASURED AND FILED SEPARATELY (NOT fixed here — see the filed
// follow-up issue quoted in this lane's report):
//   - The issue's OWN literal repro — `var outer: [1]IHolder :=
//     [new RecHolder(...)];`, a LOCAL-DECL INITIALIZER — has its OWN,
//     separate per-element store loop in `Emitter::lowerLocalDecl`
//     (LlvmBackend_Setup.cpp), which needs the IDENTICAL one-line
//     `boxForTarget` insertion. That file is HELD for another lane's
//     concurrent work this cycle, so this fix could not reach it; it still
//     SIGSEGVs (measured, both pre- and post-this-fix). Likewise a plain
//     (non-property) FIELD/auto-property inline-array-of-interface
//     initializer holding a record, and a record's primary-constructor
//     argument of that shape, both plausibly share the same Setup.cpp
//     defect family but were not independently re-traced here (out of this
//     lane's file scope; flagged in the follow-up issue for verification
//     alongside the fix).
//   - #4310 (sema): a NON-conforming class silently accepted into an
//     interface-typed collection element. Unrelated axis (rejection of an
//     invalid program vs. a crash on a valid one) — already fixed,
//     verified unaffected by this commit (that suite still passes 17/17).
//   - #4316: a plain `AssignmentStmt` has no ADMISSIBILITY gate for a
//     NON-conforming element at all (a sema gap). This file only exercises
//     GENUINELY CONFORMING elements through the assignment-statement path,
//     so it does not touch or depend on #4316's fix.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// `IHolder` (one method), a genuinely conforming record `RecHolder` (§9.6 —
// a record may implement an interface declaring no settable member), and a
// genuinely conforming class `Widget` (the accept-side-control twin proving
// the fix's `boxForTarget` no-op path for an already-boxed/bare-pointer
// class element).
const std::string kPrelude =
    "module M; "
    "interface IHolder method GetV(): string; end IHolder; "
    "record RecHolder(V: string): IHolder "
    "  public method GetV(): string => V; "
    "end RecHolder; "
    "class Widget: IHolder "
    "  private _s: string; "
    "  public constructor(s: string) _s := s; end constructor; "
    "  public method GetV(): string return _s; end GetV; "
    "end Widget; ";

JitResult run(const std::string& mainBody,
              const std::string& extraMember = "") {
    return runAot(kPrelude +
                  "class C public static method Main(): int " +
                  mainBody + " end Main; " + extraMember + " end C;");
}

}  // namespace

// ============================================================
// FIXED ROWS — each SIGSEGVs (exit 139) without the fix and returns the
// correct value with it. "he" + "llo" mirrors #4310's style: a runtime-
// concatenated string defeats any compile-time constant-folding shortcut.
// ============================================================

// Site 1: `outer[i] := new RecHolder(...)` — single-element index-assignment
// into a pre-existing `[N owned]IHolder`.
// NOTE: switched to `[1 owned]` (the spec-conformant form per §15.1.1 /
// #4336 / `569a680`): the non-owning `[1]IHolder` is a BORROW slot per §15.1
// and the `new Widget("init")` initializer is now rejected by #4351's sema
// fix. The owning form takes ownership of the fresh widget on construction
// and exercises the SAME index-assign boxing path this test pins.
BEZEL_TEST(inline_array_interface_record_element_store_boxing_4317,
           index_assign_record_element_boxed_spec_15_1_8_6) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var outer: [1 owned]IHolder := [new Widget(\"init\")]; "
        "outer[0] := new RecHolder(s); "
        "return outer[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// Same site, a SECOND slot of a `[2 owned]IHolder` — proves the fix is not
// slot-0-specific.
// See note above: switched to `[2 owned]` for #4351.
BEZEL_TEST(inline_array_interface_record_element_store_boxing_4317,
           index_assign_record_element_second_slot_boxed_spec_15_1_8_6) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var outer: [2 owned]IHolder := "
        "  [new Widget(\"init0\"), new Widget(\"init1\")]; "
        "outer[1] := new RecHolder(s); "
        "return outer[1].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// Site 2: `outer := [new RecHolder(...), ...]` — whole-array collection-
// literal REASSIGNMENT (a plain `AssignmentStmt`, not a declaration).
// See note above: switched to `[1 owned]` for #4351.
BEZEL_TEST(inline_array_interface_record_element_store_boxing_4317,
           assignment_stmt_whole_literal_record_element_boxed_spec_15_1_8_6) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var outer: [1 owned]IHolder := [new Widget(\"init\")]; "
        "outer := [new RecHolder(s)]; "
        "return outer[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// Same site, TWO record elements plus a mix with a class element in one
// literal — proves the per-element loop boxes every record slot
// independently rather than only the first.
// See note above: switched to `[2 owned]` for #4351.
BEZEL_TEST(
    inline_array_interface_record_element_store_boxing_4317,
    assignment_stmt_whole_literal_mixed_class_and_record_boxed_spec_15_1_8_6) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var outer: [2 owned]IHolder := [new Widget(\"init0\"), "
        "                              new Widget(\"init1\")]; "
        "outer := [new Widget(\"cls\"), new RecHolder(s)]; "
        "return outer[0].GetV().Length + outer[1].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 8);  // "cls".Length(3) + "hello".Length(5)
}

// Site 3: `return [new RecHolder(...)];` from a method declared to return
// `[1 owned]IHolder` — the ReturnStmt collection-literal arm.
// See note above: switched to `[1 owned]` for #4351.
BEZEL_TEST(inline_array_interface_record_element_store_boxing_4317,
           return_stmt_inline_array_literal_record_element_boxed_spec_15_1_8_6) {
    auto r = runAot(
        kPrelude +
        "class Helper public static method Wrap(s: string): [1 owned]IHolder "
        "  return [new RecHolder(s)]; end Wrap; end Helper; "
        "class C public static method Main(): int "
        "  var s := \"he\" + \"llo\"; "
        "  var outer: [1 owned]IHolder := Helper.Wrap(s); "
        "  return outer[0].GetV().Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ============================================================
// ACCEPT-SIDE CONTROLS — the same three call sites with a CLASS element
// (already a bare pointer; `boxForTarget` must no-op) must stay correct.
// A false change here would mean the fix's no-op guard is wrong.
// ============================================================

// See note above: switched to `[1 owned]` for #4351.
BEZEL_TEST(inline_array_interface_record_element_store_boxing_4317,
           index_assign_class_element_unaffected_control) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var outer: [1 owned]IHolder := [new Widget(\"init\")]; "
        "outer[0] := new Widget(s); "
        "return outer[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// See note above: switched to `[1 owned]` for #4351.
BEZEL_TEST(inline_array_interface_record_element_store_boxing_4317,
           assignment_stmt_whole_literal_class_element_unaffected_control) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var outer: [1 owned]IHolder := [new Widget(\"init\")]; "
        "outer := [new Widget(s)]; "
        "return outer[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// See note above: switched to `[1 owned]` for #4351.
BEZEL_TEST(inline_array_interface_record_element_store_boxing_4317,
           return_stmt_class_element_unaffected_control) {
    auto r = runAot(
        kPrelude +
        "class Helper public static method Wrap(s: string): [1 owned]IHolder "
        "  return [new Widget(s)]; end Wrap; end Helper; "
        "class C public static method Main(): int "
        "  var s := \"he\" + \"llo\"; "
        "  var outer: [1 owned]IHolder := Helper.Wrap(s); "
        "  return outer[0].GetV().Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// A scalar `IHolder`-typed index-assignment target (not an inline array at
// all — a plain local) must stay on the generic lvalueAddr fallback's own
// pre-existing `boxForTarget` call, unaffected by this IndexExpr-arm-
// specific fix.
BEZEL_TEST(inline_array_interface_record_element_store_boxing_4317,
           scalar_local_record_reassignment_unaffected_control) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var h: IHolder := new Widget(\"init\"); "
        "h := new RecHolder(s); "
        "return h.GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}
