// fix #4427 / #4335 / #4317 (5th call site) / spec §15.1 *Inline arrays* /
// §8.6 *Boxing* (v0.298). §8.6 states, verbatim: "When a value type is
// used where `Object` (or an interface implemented by the value type) is
// expected, the value is **boxed**: a heap-allocated wrapper holds the
// value." §15.1 constructs an inline array's slots from a collection
// literal or an element-position store; nothing in §15.1 exempts an
// interface-typed slot from §8.6's box obligation.
//
// ROOT CAUSE (traced, LlvmBackend_Setup.cpp). `#4335`'s fix (commit
// `c0d4cadf`, `narrow #4335`) closed the SIGSEGV at the LOCAL-DECL-INIT
// branch in `Emitter::lowerLocalDecl` — the only store site living in
// Setup.cpp at the time. The SIBLING — the FIELD-INITIALIZER and
// auto-PROPERTY-INITIALIZER inline-array branch in
// `Emitter::emitOwnFieldInit` (LlvmBackend_Setup.cpp ~11590, a different
// per-element store loop that #4335's commit deliberately did not
// touch) — still SIGSEGVs. Layer: STORE/INITIALIZATION, identical to
// #4335 / #4317. Measured under plain execution on the issue's own
// repro: `Segmentation fault (core dumped)`, exit 139 — i.e. the
// record's raw (unboxed) value bytes, a single 8-byte string fat-
// pointer word for `RecHolder(V: string)`, were stored directly into
// the box-pointer-shaped slot, and a later `.GetV()` dispatch read
// those bytes as `{vtable*, rc, payload}` and walked off the tiny
// string global. The same helper the working scalar-local and heap-
// `Array<T>` paths already use, `boxForTarget(value, elementSemaTy,
// sourceExpr)` (LlvmBackend_Layout.cpp:3347), is the missing call at
// this fifth site.
//
// THE FIX is one `boxForTarget` call inserted in `emitOwnFieldInit`'s
// per-element store loop (LlvmBackend_Setup.cpp), exactly mirroring the
// three sites #4317 already added in Stmts.cpp and the fourth site
// #4335 added in `lowerLocalDecl`. The element's DECLARED sema type is
// read off the field's resolved `TypeRef` (already in scope as
// `fieldRt` after the #2874 imported-fallback block, so no re-resolve
// is needed here). The array's `InlineArray` kind's `arrayElement`
// field is the interface `IHolder` — exactly the `declTy`
// `boxForTarget` needs to classify an interface target. `fieldRt`
// itself cannot be null here: the whole branch is gated on `if
// (fieldRt) { … }` (line ~11565), and the `arrayElement` read is
// gated on `fieldRt->kind == InlineArray` (defense in depth).
//
// SCOPE (intentionally narrow — this lane fixes only the FIELD/PROPERTY-
// INIT sibling of #4335's LOCAL-DECL-INIT fix):
//   - #4310 (sema): a NON-conforming class silently accepted into an
//     interface-typed collection element. Unrelated axis. This file
//     does NOT include a paired REJECT row for that predicate — the
//     sibling sema gate was exercised on the local-decl-init shape by
//     the #4335 file's `local_decl_init_noconform_record_rejected_*`
//     row (sema rejects at the SHARED inline-array collection-literal
//     position, not at a field/property specific one), so a duplicate
//     here would only test the same sema predicate twice. The brief's
//     field-init / property-init focus is covered by the FIXED rows
//     (both a plain field `Items: [1]IHolder := [...]` and an auto-
//     property `property Items: [1]IHolder := [...]` use the same
//     `emitOwnFieldInit` walk).
//   - #4316 / #4336 (drop-on-overwrite / assignment-stmt admissibility
//     gaps): not in scope here; both are filed separately and not
//     affected by this one-line fix.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// `IHolder` (one method), a genuinely CONFORMING record `RecHolder`
// (§9.6 — a record may implement an interface declaring no settable
// member), and a genuinely CONFORMING class `Widget` (the accept-side-
// control twin proving the fix's `boxForTarget` no-op path for an
// already-boxed / bare-pointer class element). The Holder class is
// declared as the user-defined enclosing type for the FIXED field/property
// rows — the issue's literal repro is `class Holder { public Items:
// [1]IHolder := [new RecHolder("x")]; }` and the §15.1 site under
// test is `emitOwnFieldInit`'s walk over `Holder.members`.
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
    "end Widget; "
    "class HolderField "
    "  public Items: [1]IHolder := [new RecHolder(\"x\")]; "
    "end HolderField; "
    "class HolderProp "
    "  public property Items: [1]IHolder := [new RecHolder(\"y\")]; "
    "end HolderProp; "
    "class C ";

const std::string kEpilogue = " end C;";

// Each row builds the full source as: kPrelude + <type decl(s) + class
// Main decl> + kEpilogue. Two flavors: field-row wraps `body` in a
// `class C { public static method Main(): int { … } end Main; }` shape;
// property-row does the same. The body accesses `HolderField` or
// `HolderProp` accordingly. Concatenated strings defeat any compile-time
// constant-folding shortcut, matching the #4335 file's idiom.
JitResult runField(const std::string& body) {
    return runAot(kPrelude +
                  "public static method Main(): int " + body +
                  " end Main; " + kEpilogue);
}

}  // namespace

// ============================================================
// FIXED ROWS — each SIGSEGVs (exit 139) without the fix and
// returns the correct value with it. The literal repro from
// issue #4427 is the first row.
// ============================================================

// The issue's OWN literal repro — a plain (non-property) FIELD
// initializer: `class Holder { public Items: [1]IHolder := [new
// RecHolder("x")]; }`, read via `.Items[0].GetV().Length`.
BEZEL_TEST(inline_array_interface_record_field_init_4427,
           field_init_record_element_boxed_spec_15_1_8_6) {
    auto r = runField(
        "var h: HolderField := new HolderField(); "
        "return h.Items[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// An auto-PROPERTY initializer twin of the field-init row above —
// `property Items: [1]IHolder := [new RecHolder("y")];` is the
// `PropertyDecl` flavor of the same shape, and exercises the SAME
// `emitOwnFieldInit` walk via `pd->autoInitializer` (CG33). Both
// routes feed the inline-array collection-literal branch — proving
// the fix lives in `emitOwnFieldInit`, not in any property-specific
// lowering.
BEZEL_TEST(inline_array_interface_record_field_init_4427,
           auto_property_init_record_element_boxed_spec_15_1_8_6) {
    auto r = runField(
        "var h: HolderProp := new HolderProp(); "
        "return h.Items[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// A TWO-slot inline array on a field — proves the fix is not slot-0-
// specific. Both elements are records of different payloads so a
// "first element only" defect would still slip a length from the
// second into the first.
BEZEL_TEST(
    inline_array_interface_record_field_init_4427,
    field_init_record_element_second_slot_boxed_spec_15_1_8_6) {
    const std::string prelude2 =
        "module M; "
        "interface IHolder method GetV(): string; end IHolder; "
        "record RecHolder(V: string): IHolder "
        "  public method GetV(): string => V; "
        "end RecHolder; "
        "class Holder2 "
        "  public Items: [2]IHolder := "
        "    [new RecHolder(\"he\" + \"llo\"), new RecHolder(\"world\")]; "
        "end Holder2; "
        "class C ";
    auto r = runAot(prelude2 +
                    "public static method Main(): int "
                    "  var h: Holder2 := new Holder2(); "
                    "  return h.Items[1].GetV().Length; "
                    "end Main; " + kEpilogue);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// A class element + a record element in the SAME field-init literal —
// the class slot must no-op `boxForTarget` and the record slot must
// box. Exercises the per-element loop hitting the new `boxForTarget`
// call on every iteration (not just the first) — proving the fix lives
// in the loop body, not at literal-construction time.
//
// #4351 RETARGET (the CLASS element only). `[2]IHolder` is a NON-owning
// inline array, so per §15.1 (v0.306) "the initializer expressions must
// be borrows of class instances already owned elsewhere, and those
// owners must outlive the inline array" — a `new Widget(...)` element is
// a fresh producer with no outer owner and `f837b164` correctly rejects
// it. The RECORD element is NOT retargeted: `new RecHolder(...)` at an
// interface slot **boxes** (§8.6) and a box is runtime-managed (§28.9),
// so §15.1's borrow contract has nothing to bind — that is the axis this
// row exists to pin and it must keep running on the NON-owning shape.
// `f837b164`'s `using var w := …; [w]` recipe is unavailable at a FIELD
// initializer (no local is in scope), so the owner is a `static owned
// readonly` field instead. The authority for that being a sufficient
// owner is §28.3's producers table — "Static field access `T.f` … with
// class- or interface-typed result | Borrowed (source: the declaring
// type's static storage — K2's always-live source … disposed only at
// program exit, §22.13)" — resting on kernel K2's named-location bullet,
// which names static storage "the one distinguished *always-live*
// source". (NOT §28.3 "clause 2": §28.3's borrow-lifetime rules are an
// unnumbered bullet list; the numbered clauses 1–4 are K2's, and its
// clause 2 is Union under composition.)
//
// This program emits a §22.12 rule 4 warning on the owner field
// ("static readonly field `W` has class type `Widget` which is
// reachable from every domain …", #1363). Expected, and inert here:
// nothing in this file asserts on diagnostics.
BEZEL_TEST(
    inline_array_interface_record_field_init_4427,
    field_init_mixed_class_and_record_boxed_spec_15_1_8_6) {
    const std::string prelude3 =
        "module M; "
        "interface IHolder method GetV(): string; end IHolder; "
        "record RecHolder(V: string): IHolder "
        "  public method GetV(): string => V; "
        "end RecHolder; "
        "class Widget: IHolder "
        "  private _s: string; "
        "  public constructor(s: string) _s := s; end constructor; "
        "  public method GetV(): string return _s; end GetV; "
        "end Widget; "
        "class WidgetHome "
        "  public static owned readonly W: Widget := new Widget(\"cls\"); "
        "end WidgetHome; "
        "class Holder3 "
        "  public Items: [2]IHolder := "
        "    [WidgetHome.W, new RecHolder(\"he\" + \"llo\")]; "
        "end Holder3; "
        "class C ";
    auto r = runAot(prelude3 +
                    "public static method Main(): int "
                    "  var h: Holder3 := new Holder3(); "
                    "  return h.Items[0].GetV().Length + "
                    "         h.Items[1].GetV().Length; "
                    "end Main; " + kEpilogue);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 8);  // "cls".Length(3) + "hello".Length(5)
}

// ============================================================
// ACCEPT-SIDE CONTROLS — the same call site with a CLASS element
// (already a bare pointer; `boxForTarget` must no-op) and with non-
// interface element types must stay correct. A false change here
// would mean the fix's no-op guard is wrong.
// ============================================================

// A genuinely conforming class as a single inline-array element at
// the field init — this was already correct pre-fix and must stay
// correct. The field's slot holds a bare pointer; `boxForTarget`'s
// `isBoxableValueType` predicate on the source returns false for a
// class expression, so the value flows through unboxed.
//
// #4351 RETARGET. The element was `new Widget("hello")`, which §15.1
// (v0.306) rejects at a NON-owning `[1]IHolder` slot — the slot stores a
// borrow and a fresh producer has no outer owner. Retargeted to the
// spec's own sanctioned shape, "a borrow of a class instance already
// owned elsewhere": a `static owned readonly` field. §28.3's producers
// table classifies a static field read as Borrowed with the declaring
// type's static storage as its source — kernel K2's "one distinguished
// *always-live* source" — and §22.13 disposes it at program exit. (See
// the note on the mixed row above for why this is NOT "§28.3 clause 2",
// and for the expected §22.12 rule 4 warning on the owner field.)
// The NON-owning `[1]IHolder` field shape — the `emitOwnFieldInit`
// per-element store loop this file exists to pin — is unchanged, which
// `[1 owned]IHolder` would not have preserved.
BEZEL_TEST(inline_array_interface_record_field_init_4427,
           field_init_class_element_unaffected_control) {
    const std::string preludeC =
        "module M; "
        "interface IHolder method GetV(): string; end IHolder; "
        "class Widget: IHolder "
        "  private _s: string; "
        "  public constructor(s: string) _s := s; end constructor; "
        "  public method GetV(): string return _s; end GetV; "
        "end Widget; "
        "class WidgetHome "
        "  public static owned readonly W: Widget := "
        "    new Widget(\"he\" + \"llo\"); "
        "end WidgetHome; "
        "class HolderC "
        "  public Items: [1]IHolder := [WidgetHome.W]; "
        "end HolderC; "
        "class C ";
    auto r = runAot(preludeC +
                    "public static method Main(): int "
                    "  var h: HolderC := new HolderC(); "
                    "  return h.Items[0].GetV().Length; "
                    "end Main; " + kEpilogue);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// A non-interface element type (`[3]int`) on a field —
// `boxForTarget`'s `isObjectOrInterfaceTarget` guard must reject this
// as a no-op target, so the integers pass through unboxed.
BEZEL_TEST(inline_array_interface_record_field_init_4427,
           field_init_int_element_unaffected_control) {
    const std::string preludeI =
        "module M; "
        "class HolderI "
        "  public Items: [3]int := [10, 20, 30]; "
        "end HolderI; "
        "class C ";
    auto r = runAot(preludeI +
                    "public static method Main(): int "
                    "  var h: HolderI := new HolderI(); "
                    "  return h.Items[0] + h.Items[1] + h.Items[2]; "
                    "end Main; " + kEpilogue);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}
