// fix #4467 / spec §15.1 *Inline arrays* / §8.6 *Boxing* (v0.298).
// §8.6 states, verbatim: "When a value type is used where `Object` (or an
// interface implemented by the value type) is expected, the value is
// **boxed**: a heap-allocated wrapper holds the value." §15.1 constructs an
// inline array's slots from a collection literal; nothing in §15.1 exempts a
// STATIC-INIT inline-array-of-interface slot from §8.6's box obligation.
//
// ROOT CAUSE (traced, LlvmBackend_Setup.cpp). #4317's fix (commit `4cde65e`)
// closed the SIGSEGV at three of four call sites; #4335 (commit `a9888162`)
// closed the 4th in `Emitter::lowerLocalDecl`. The remaining sibling — a
// STATIC `readonly` field initialised from a collection literal of conforming
// records — lives in a different region of the same file (the runtime-init
// branch of the static-init emission, the path that fires when
// `isBakedStaticFieldInit` returns false — i.e. when the initializer is NOT
// a plain constant literal of a primitive/string kind). The per-element
// store loop calls `lowerExpr(*cl->elements[i])`, applies an integer-only
// width adjustment, then stores the bare record bytes into the box-pointer-
// shaped iface slot. Layer: STORE/INITIALIZATION, identical to #4317/#4335
// — measured under ASan on the issue's own repro:
// `global-buffer-overflow`, READ of size 8, N bytes past the string
// constant — i.e. the record's raw (unboxed) value bytes, a single 8-byte
// string fat-pointer word for `RecHolder(V: string)`, were stored directly
// into the box-pointer-shaped slot, and a later `.GetV()` dispatch read
// those bytes as `{vtable*, rc, payload}` and walked off the tiny string
// global. The same helper the working local-decl-init path already uses,
// `boxForTarget(value, elementSemaTy, sourceExpr)` (LlvmBackend_Layout.cpp),
// is the missing call at this 5th site.
//
// THE FIX is one `boxForTarget` call inserted in the static-init runtime
// branch's per-element store loop (LlvmBackend_Setup.cpp, immediately after
// the existing width-adjustment guard). The element's DECLARED sema type
// is read off the field's resolved `TypeRef` (the array's `InlineArray`
// kind's `arrayElement` field), mirroring the local-decl-init site's
// `declType->arrayElement` pattern.
//
// SCOPE (intentionally narrow — this lane fixes only the 5th sibling, the
// static-init inline-array branch):
//   - The instance-field-init and auto-PROPERTY-init paths (also living in
//     Setup.cpp) have their own per-element collection-literal loops and
//     are addressed by #4427 (OPEN). NOT verified here.
//   - #4310 (sema): a NON-conforming class silently accepted into an
//     interface-typed collection element. Unrelated axis. The companion
//     REJECT-AND-COMPILE-CLEAN row in this file proves sema still gates
//     the static-init shape (#4310 covered it explicitly).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// `IHolder` (one method), a genuinely CONFORMING record `RecHolder`
// (§9.6 — a record may implement an interface declaring no settable
// member), a genuinely CONFORMING class `Widget` (the accept-side-control
// twin proving the fix's `boxForTarget` no-op path for an already-boxed /
// bare-pointer class element), and a NON-conforming record `NoHolder`
// (never lists `IHolder` — used to assert that the sibling sema gate
// #4310 still rejects a not-implementing record at the static-init
// inline-array position).
const std::string kPrelude =
    "module M; "
    "interface IHolder method GetV(): string; end IHolder; "
    "record RecHolder(V: string): IHolder "
    "  public method GetV(): string => V; "
    "end RecHolder; "
    "record NoHolder(X: int) "
    "  public method GetX(): int => X; "
    "end NoHolder; "
    "class Widget: IHolder "
    "  private _s: string; "
    "  public constructor(s: string) _s := s; end constructor; "
    "  public method GetV(): string return _s; end GetV; "
    "end Widget; ";

JitResult run(const std::string& storeDecl,
              const std::string& mainBody,
              const std::string& extraMember = "") {
    return runAot(kPrelude + storeDecl +
                  "class C public static method Main(): int " +
                  mainBody + " end Main; " + extraMember + " end C;");
}

}  // namespace

// ============================================================
// FIXED ROWS — each SIGSEGVs (exit 139) without the fix and returns the
// correct value with it. "he" + "llo" mirrors #4335's style: a runtime-
// concatenated string defeats any compile-time constant-folding shortcut.
// ============================================================

// The issue's OWN literal repro — a `static readonly` field of an inline
// array of an interface, initialised from a record collection literal,
// accessed via `GetV().Length` from `Main`. (Issue #4467, repro block.)
BEZEL_TEST(inline_array_interface_record_static_init_4467,
           static_init_record_element_boxed_spec_15_1_8_6) {
    auto r = run(
        "class StaticStore "
        "  public static readonly _items: [1]IHolder := "
        "    [new RecHolder(\"he\" + \"llo\")]; "
        "end StaticStore; ",
        "return StaticStore._items[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// Same shape, a SECOND slot of a `[2]IHolder` — proves the fix is not
// slot-0-specific. Both elements are records of different payloads so a
// "first element only" defect would still slip a length from the second
// into the first.
BEZEL_TEST(
    inline_array_interface_record_static_init_4467,
    static_init_record_element_second_slot_boxed_spec_15_1_8_6) {
    auto r = run(
        "class StaticStore2 "
        "  public static readonly _items: [2]IHolder := "
        "    [new RecHolder(\"he\" + \"llo\"), new RecHolder(\"world\")]; "
        "end StaticStore2; ",
        "return StaticStore2._items[1].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// A class element + a record element in the SAME static-init literal —
// the class slot must no-op `boxForTarget` and the record slot must box.
//
// #4351 RETARGET (the CLASS element only). `[2]IHolder` is a NON-owning
// inline array, so per §15.1 (v0.306) "the initializer expressions must
// be borrows of class instances already owned elsewhere" — the former
// `new Widget("cls")` element is a fresh producer with no outer owner
// and `f837b164` correctly rejects it. The RECORD element stays a `new`:
// it **boxes** at the interface slot (§8.6) and a box is runtime-managed
// (§28.9), so §15.1's borrow contract has nothing to bind — that is the
// axis this row pins, and it must keep running on the NON-owning shape.
// The class owner is a `static owned readonly` field. Its authority as a
// sufficient owner is §28.3's producers table — "Static field access
// `T.f` … with class- or interface-typed result | Borrowed (source: the
// declaring type's static storage — K2's always-live source … disposed
// only at program exit, §22.13)" — resting on kernel K2's named-location
// bullet, which names static storage "the one distinguished *always-live*
// source". (NOT §28.3 "clause 2": §28.3's borrow-lifetime rules are an
// unnumbered bullet list; the numbered clauses 1–4 are K2's.)
//
// ORDERING is NOT declaration order. §22.13: "Within a single type,
// static fields are initialized in textual declaration order. Across
// types, a type's static fields are initialized lazily on first use."
// `WidgetHome` and `StaticStore3` are distinct types, so their textual
// order is irrelevant — the ordering comes from the DEPENDENCY: reading
// `WidgetHome.W` inside `StaticStore3`'s initializer is the first use
// that triggers WidgetHome's lazy initialization, at that point. §22.13's
// cross-type dependency graph is what keeps that edge acyclic. Measured:
// declaring WidgetHome AFTER StaticStore3 compiles and still returns 8.
//
// This program emits a §22.12 rule 4 warning on the owner field
// ("static readonly field `W` has class type `Widget` which is
// reachable from every domain …", #1363). Expected, and inert here:
// nothing in this row asserts on diagnostics.
BEZEL_TEST(
    inline_array_interface_record_static_init_4467,
    static_init_mixed_class_and_record_boxed_spec_15_1_8_6) {
    auto r = run(
        "class WidgetHome "
        "  public static owned readonly W: Widget := new Widget(\"cls\"); "
        "end WidgetHome; "
        "class StaticStore3 "
        "  public static readonly _items: [2]IHolder := "
        "    [WidgetHome.W, new RecHolder(\"he\" + \"llo\")]; "
        "end StaticStore3; ",
        "return StaticStore3._items[0].GetV().Length + "
        "       StaticStore3._items[1].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 8);  // "cls".Length(3) + "hello".Length(5)
}

// Two records of differing payload in a single static-init literal.
// Exercises the per-element loop hitting the new `boxForTarget` call on
// every iteration (not just the first) — proving the fix lives in the
// loop body, not at literal-construction time.
BEZEL_TEST(
    inline_array_interface_record_static_init_4467,
    static_init_two_record_elements_both_boxed_spec_15_1_8_6) {
    auto r = run(
        "class StaticStore4 "
        "  public static readonly _items: [2]IHolder := "
        "    [new RecHolder(\"he\" + \"llo\"), new RecHolder(\"world\")]; "
        "end StaticStore4; ",
        "return StaticStore4._items[0].GetV().Length + "
        "       StaticStore4._items[1].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);  // "hello".Length(5) + "world".Length(5)
}

// ============================================================
// REJECT ROW — sibling sema #4310 still gates the same shape for a NOT-
// conforming element. A plain (non-conforming) record at this position
// must keep being rejected with the "not implicitly convertible" / §5.1
// diagnostic — a false-pass here would mean this fix accidentally
// admitted an invalid program.
// ============================================================

bool hasStaticInitNotImplicitlyConvertible(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos &&
            d.message.find("5.1") != std::string::npos)
            return true;
    return false;
}

BEZEL_TEST(inline_array_interface_record_static_init_4467,
           static_init_noconform_record_rejected_spec_5_1_11_2) {
    auto r = run(
        "class StaticStoreBad "
        "  public static readonly _items: [1]IHolder := [new NoHolder(7)]; "
        "end StaticStoreBad; ",
        "return (if StaticStoreBad._items[0] = null then 0 else 1);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasStaticInitNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ============================================================
// ACCEPT-SIDE CONTROLS — the same call site with a CLASS element (already
// a bare pointer; `boxForTarget` must no-op) and with non-interface
// element types must stay correct. A false change here would mean the
// fix's no-op guard is wrong.
// ============================================================

// A genuinely conforming class as a single inline-array element at the
// static init — this was already correct pre-fix and must stay correct.
//
// #4351 RETARGET. The element was `new Widget("hello")`, which §15.1
// (v0.306) rejects at a NON-owning `[1]IHolder` slot — the slot stores a
// borrow and a fresh producer has no outer owner. Retargeted to the
// spec's own sanctioned shape, "a borrow of a class instance already
// owned elsewhere": a `static owned readonly` field. §28.3's producers
// table classifies a static field read as Borrowed with the declaring
// type's static storage as its source — kernel K2's "one distinguished
// *always-live* source" — and §22.13 disposes it at program exit. (See
// the mixed row above for why this is NOT "§28.3 clause 2", for the
// lazy-on-first-use cross-type ordering, and for the expected §22.12
// rule 4 warning on the owner field.)
// The NON-owning `[1]IHolder` static-init shape — the runtime-init branch's
// per-element store loop this file exists to pin — is unchanged, which
// `[1 owned]IHolder` would not have preserved.
BEZEL_TEST(inline_array_interface_record_static_init_4467,
           static_init_class_element_unaffected_control) {
    auto r = run(
        "class WidgetHome "
        "  public static owned readonly W: Widget := "
        "    new Widget(\"he\" + \"llo\"); "
        "end WidgetHome; "
        "class StaticStoreCls "
        "  public static readonly _items: [1]IHolder := [WidgetHome.W]; "
        "end StaticStoreCls; ",
        "return StaticStoreCls._items[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// A non-interface element type (`[1]int`) — `boxForTarget`'s
// `isObjectOrInterfaceTarget` guard must reject this as a no-op target,
// so the integer passes through unboxed.
BEZEL_TEST(inline_array_interface_record_static_init_4467,
           static_init_int_element_unaffected_control) {
    auto r = run(
        "class StaticStoreInt "
        "  public static readonly _items: [3]int := [10, 20, 30]; "
        "end StaticStoreInt; ",
        "return StaticStoreInt._items[0] + StaticStoreInt._items[1] + "
        "       StaticStoreInt._items[2];");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}
