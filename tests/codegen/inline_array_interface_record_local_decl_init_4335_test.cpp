// fix #4335 / #4317 (4th call site) / spec §15.1 *Inline arrays* /
// §8.6 *Boxing* (v0.298). §8.6 states, verbatim: "When a value type is
// used where `Object` (or an interface implemented by the value type) is
// expected, the value is **boxed**: a heap-allocated wrapper holds the
// value." §15.1 constructs an inline array's slots from a collection
// literal or an element-position store; nothing in §15.1 exempts an
// interface-typed slot from §8.6's box obligation.
//
// ROOT CAUSE (traced, LlvmBackend_Setup.cpp). `#4317`'s fix (commit
// `4cde65e`, `narrow #4317`) closed the SIGSEGV at three of four call
// sites in `LlvmBackend_Stmts.cpp` (IndexExpr-arm index-assign,
// whole-array-literal reassign, return-stmt). The fourth — the
// LOCAL-DECL-INITIALIZER inline-array branch in `Emitter::lowerLocalDecl`
// (the only store site living in Setup.cpp) — was deliberately held
// because Setup.cpp was held for another lane's concurrent work
// (#4315/#4322) that cycle, so the fix could not reach it; it still
// SIGSEGVs. Layer: STORE/INITIALIZATION, identical to #4317 — measured
// under ASan on the issue's own repro: `global-buffer-overflow`, READ of
// size 8, 2 bytes past `.bz.str.data.53` (the "hello" string constant) —
// i.e. the record's raw (unboxed) value bytes, a single 8-byte string
// fat-pointer word for `RecHolder(V: string)`, were stored directly into
// the box-pointer-shaped slot, and a later `.GetV()` dispatch read those
// bytes as `{vtable*, rc, payload}` and walked off the tiny string
// global. The same helper the working scalar-local and heap-`Array<T>`
// paths already use, `boxForTarget(value, elementSemaTy, sourceExpr)`
// (LlvmBackend_Layout.cpp), is the missing call at this fourth site.
//
// THE FIX is one `boxForTarget` call inserted in `lowerLocalDecl`'s
// per-element store loop (LlvmBackend_Setup.cpp), exactly mirroring the
// three sites #4317 already added in Stmts.cpp. The element's DECLARED
// sema type is read off `declType`'s resolved `TypeRef` (the array's
// `InlineArray` kind's `arrayElement` field) — the inference fallback
// (`declType == nullptr`) passes `nullptr` to `boxForTarget`, which is a
// safe no-op per the helper's own guard (returns v unchanged on a null
// declTy), AND an inferred array type could not have deduced an
// interface element from a `new RecHolder(...)` first element in the
// first place, so the only case that matters (the declared-element-type
// case) is covered.
//
// SCOPE (intentionally narrow — this lane fixes only the 4th #4317 site):
//   - #4310 (sema): a NON-conforming class silently accepted into an
//     interface-typed collection element. Unrelated axis. This file
//     includes a paired REJECT-AND-COMPILE-CLEAN row using a non-
//     conforming record-style holder to confirm sema still gates the
//     local-decl-init shape (#4310 covered it explicitly).
//   - #4316 / #4336 (drop-on-overwrite / assignment-stmt admissibility
//     gaps): not in scope here; both are filed separately and not
//     affected by this one-line fix.
//   - The sibling FIELD-init and auto-PROPERTY-init local-decl init
//     paths (also living in Setup.cpp, plausibly the same defect family):
//     NOT verified here — the issue body flags them as "worth checking"
//     once Setup.cpp is free to edit, but does not measure them. A lane
//     that fixes one of them should add a sibling row to this file's
//     pattern.

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
// #4310 still rejects a not-implementing record at the local-decl-init
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

// The issue's OWN literal repro — local-decl init of an inline-array-of-
// interface literal whose element is a genuinely conforming record.
BEZEL_TEST(inline_array_interface_record_local_decl_init_4335,
           local_decl_init_record_element_boxed_spec_15_1_8_6) {
    auto r = run(
        "var outer: [1]IHolder := [new RecHolder(\"he\" + \"llo\")]; "
        "return outer[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// Same shape, a SECOND slot of a `[2]IHolder` — proves the fix is not
// slot-0-specific. Both elements are records of different payloads so a
// "first element only" defect would still slip a length from the second
// into the first.
BEZEL_TEST(
    inline_array_interface_record_local_decl_init_4335,
    local_decl_init_record_element_second_slot_boxed_spec_15_1_8_6) {
    auto r = run(
        "var outer: [2]IHolder := "
        "  [new RecHolder(\"he\" + \"llo\"), new RecHolder(\"world\")]; "
        "return outer[1].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// A class element + a record element in the SAME local-decl-init literal —
// the class slot must no-op `boxForTarget` and the record slot must box.
BEZEL_TEST(
    inline_array_interface_record_local_decl_init_4335,
    local_decl_init_mixed_class_and_record_boxed_spec_15_1_8_6) {
    auto r = run(
        "var outer: [2]IHolder := "
        "  [new Widget(\"cls\"), new RecHolder(\"he\" + \"llo\")]; "
        "return outer[0].GetV().Length + outer[1].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 8);  // "cls".Length(3) + "hello".Length(5)
}

// Two records of differing payload in a single local-decl-init literal.
// Exercises the per-element loop hitting the new `boxForTarget` call on
// every iteration (not just the first) — proving the fix lives in the
// loop body, not at literal-construction time.
BEZEL_TEST(
    inline_array_interface_record_local_decl_init_4335,
    local_decl_init_two_record_elements_both_boxed_spec_15_1_8_6) {
    auto r = run(
        "var outer: [2]IHolder := "
        "  [new RecHolder(\"he\" + \"llo\"), new RecHolder(\"world\")]; "
        "return outer[0].GetV().Length + outer[1].GetV().Length;");
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

bool hasNotImplicitlyConvertible(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos &&
            d.message.find("5.1") != std::string::npos)
            return true;
    return false;
}

BEZEL_TEST(inline_array_interface_record_local_decl_init_4335,
           local_decl_init_noconform_record_rejected_spec_5_1_11_2) {
    auto r = run(
        "var outer: [1]IHolder := [new NoHolder(7)]; "
        "return (if outer[0] = null then 0 else 1);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ============================================================
// ACCEPT-SIDE CONTROLS — the same call site with a CLASS element (already
// a bare pointer; `boxForTarget` must no-op) and with non-interface
// element types must stay correct. A false change here would mean the
// fix's no-op guard is wrong.
// ============================================================

// A genuinely conforming class as a single inline-array element at the
// local-decl init — this was already correct pre-fix and must stay
// correct.
BEZEL_TEST(inline_array_interface_record_local_decl_init_4335,
           local_decl_init_class_element_unaffected_control) {
    auto r = run(
        "var outer: [1]IHolder := [new Widget(\"he\" + \"llo\")]; "
        "return outer[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// A non-interface element type (`[1]int`) — `boxForTarget`'s
// `isObjectOrInterfaceTarget` guard must reject this as a no-op target,
// so the integer passes through unboxed.
BEZEL_TEST(inline_array_interface_record_local_decl_init_4335,
           local_decl_init_int_element_unaffected_control) {
    auto r = run(
        "var outer: [3]int := [10, 20, 30]; "
        "return outer[0] + outer[1] + outer[2];");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}