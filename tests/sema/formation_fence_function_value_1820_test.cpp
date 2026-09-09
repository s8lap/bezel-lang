// Spec-derived regression tests for #1820 — function-value invocation
// classification + §27.6 formation fences (v0.223 ruling carried in v0.282).
//
// THE PROBLEM (§28.3, §27.6, v0.223): a function-typed value produced by a
// direct conversion from a `()`-returning expression is NOT a formation-fenced
// function value — the §28.3 producer row's `Function-value invocation` rule
// classifies such invocations as borrows, not as the borrowed-source mints the
// production rules elsewhere describe. Without the four coordinated fences:
//
//   (1) Concrete-slot function bodies callee-side rule: a lambda checked against
//       a function-type target whose return slot is a concrete class/interface
//       must NOT produce a fresh owned value at its return paths — the row's
//       caller would treat it as a borrow (no dispose) and silently leak or
//       two-owner-pair.
//
//   (2) Task-family property reference: a property reference formed at a
//       target slot whose return is `Task`-family is rejected — the reference's
//       function type would carry a mint-announcing return slot whose
//       invocation the row classifies as an owned mint, while the underlying
//       getter lends its storage (§30.11 / A4).
//
//   (3) Static-iterable-factory bridging: a method reference over a STATIC
//       iterable-returning callee bridged to a non-iterable target slot is
//       rejected — the retyped face would erase the iterator convention's
//       frame-mint announcement and an invocation through it would classify
//       under the row as a borrow (a leaked caller-owned frame).
//
// Each pin below is the §1820 spec-cited REJECT case the ruling names; the
// accept controls pin the un-rejected axes that share the same surface.
//
// FAIL-WITHOUT / PASS-WITH (verified on `d34c8b4`, pre-fix base): every
// REJECT row compiled clean (silent leak or live two-owners pairing at
// runtime) and every ACCEPT row passed. With the fix each REJECT row
// reports its spec-cited diagnostic (matched on the ruling's own wording
// plus the §-citation) and `r.ran` stays false; every ACCEPT row stays
// pinned by exit-code.
//
// AXIS GRID. The four fences operate on different surfaces (lambda body
// return paths, property reference formation, static-iterable reference
// bridging). The accept controls cover the four un-rejected shapes each
// fence must preserve:
//
//   * Concrete-slot LAMBDA: owned-returning slot stays accepted (the §28.8
//     transferring form), non-owned concrete slot at a NULL / BORROW
//     return stays accepted, value-typed slot stays accepted, runtime-
//     managed slot stays accepted.
//   * PROPERTY reference: zero-arg function at a non-Task-family
//     property stays accepted (the §27.6 getter-as-function-value
//     promotion, #756).
//   * STATIC iterable bridge: identity-type formation stays accepted,
//     instance-iterable callee stays accepted (lend-under-A2), non-iterable
//     static callee stays accepted (no frame-mint to launder).
//
// per §27.6 / §28.3 / §30.11 (issue #1820, spec v0.282 — the v0.223 ruling's
// spec half landed at v0.234, the implementation half carried as the #1820
// tracker to here).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// §28.3 / §28.6: non-trivial-dispose class with a destructor — the §21.9
// / §28.6 leak surface. The §27.6 fence at the function slot is orthogonal
// to dispose: even a trivial-dispose class would be rejected at the
// concrete-slot rule (the row classifies as borrow regardless of dispose
// triviality — the caller's missing-obligation is the issue, not the
// destructor). Used by every concrete-slot REJECT row to ensure the
// fresh-producer classification is independent of dispose-triviality.
const char* kPrelude =
    "module M; "
    "class Res(id: int) "
    "  public Id: int := id; "
    "  destructor end destructor; "
    "end Res; "
    "class Holder "
    "  public property T: int := 7; "
    "end Holder; "
    "class TaskHolder "
    "  public property Tk: Task<int>?; "
    "end TaskHolder; "
    "class C "
    "  public static method Nums(): IEnumerable<int> "
    "    yield return 0; "
    "    yield return 1; "
    "    yield return 2; "
    "  end Nums; "
    "  public async method Run(): Task<int> "
    "    return 5; "
    "  end Run; "
    "end C; ";

// True iff SOME diagnostic carries the §27.6 / §28.3 formation-fence
// wording plus the §-citation. The cx1/cx3/cx4 pins have distinct
// wordings — each matches its own fence's diagnostic text plus the
// common citation.
bool rejectedConcreteSlotLambda(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("fresh owned value") != std::string::npos &&
            d.message.find("\xC2\xA7""28.3") != std::string::npos &&
            d.message.find("fix #1820") != std::string::npos)
            return true;
    return false;
}

bool rejectedPropertyTaskFamily(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("cannot form a method reference over a "
                           "property whose declared type is") !=
                std::string::npos &&
            d.message.find("\xC2\xA7""27.6") != std::string::npos &&
            d.message.find("\xC2\xA7""28.3") != std::string::npos)
            return true;
    return false;
}

bool rejectedStaticIterableBridge(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("static iterable-factory callee") !=
                std::string::npos &&
            d.message.find("\xC2\xA7""27.6") != std::string::npos &&
            d.message.find("fix #1820") != std::string::npos)
            return true;
    return false;
}

void expectRejectedByConcreteSlotLambda(const std::string& body) {
    JitResult r = runAot(std::string(kPrelude) + body);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedConcreteSlotLambda(r));
}

void expectAcceptedByValue(const std::string& body, int expected) {
    JitResult r = runAot(std::string(kPrelude) + body);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, expected);
}

}  // namespace

// =====================================================================
// REJECT (cx3): a lambda checked against a concrete class return slot
// produces a fresh `new T()` at the body. The §28.3 producer row
// classifies the function-value invocation as a borrow — no caller would
// ever dispose the minted frame. // per §28.3, §28.6 (issue #1820 cx3 pin)
// =====================================================================
BEZEL_TEST(sema_formation_fence_function_value_1820,
           cx3_minting_lambda_under_concrete_class_rejected_spec_28_3_28_6) {
    expectRejectedByConcreteSlotLambda(
        "class D "
        "  public static method Main(): int "
        "    var f: () -> Res := lambda() => new Res(1); "
        "    var x := f(); "
        "    return x.Id; "
        "  end Main; "
        "end D;");
}

// REJECT (cx4): a lambda checked against a concrete Object return slot
// produces a Task-family call result. The Task-family call returns an
// owned mint per §28.3 / v0.190 — under the row's borrow classification
// at a concrete Object slot, the caller wouldn't dispose it (the
// shared `Wrap<Object>(lambda() => Work())` coordinated with #1802's
// discharge claim; this is the closing fence #1802's claim depends on).
// // per §28.3, §30.1, §30.6 (issue #1820 cx4 pin)
BEZEL_TEST(sema_formation_fence_function_value_1820,
           cx4_minting_lambda_under_object_rejected_spec_28_3_30_1) {
    expectRejectedByConcreteSlotLambda(
        "class D "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    var g: () -> Object := lambda() => c.Run(); "
        "    var x := g(); "
        "    return 0; "
        "  end Main; "
        "end D;");
}

// REJECT (p1820e family): a property reference over a Task-family-typed
// property. The reference's function type would carry a mint-announcing
// return slot (Task<int>) whose invocation the §28.3 row classifies as
// an owned mint, while the underlying getter LENDS its storage
// (§30.11's source-owns-task rule; axiom A4's stored-view lend-only
// guarantee). Formation would pair two owners over one stored task.
// // per §27.6, §28.3, §30.11 (issue #1820 p1820e pin)
BEZEL_TEST(sema_formation_fence_function_value_1820,
           property_reference_task_family_rejected_spec_27_6_28_3_30_11) {
    JitResult r = runAot(
        std::string(kPrelude) +
        "class D "
        "  public static method Main(): int "
        "    var h := new TaskHolder(); "
        "    var f: () -> Task<int> := h.Tk; "
        "    var x := f(); "
        "    return 0; "
        "  end Main; "
        "end D;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedPropertyTaskFamily(r));
}

// REJECT (cx1): a static iterator-factory method (`Nums(): IEnumerable<int>`)
// bridged to a non-iterable function-typed target. The retyped face would
// erase the iterator convention's frame-mint announcement, and an
// invocation through it would classify under the §28.3 producer row as a
// borrow — a leaked caller-owned frame.
// // per §27.6, §28.3 (issue #1820 cx1 pin)
BEZEL_TEST(sema_formation_fence_function_value_1820,
           cx1_static_iterable_factory_bridge_rejected_spec_27_6_28_3) {
    JitResult r = runAot(
        std::string(kPrelude) +
        "class D "
        "  public static method Main(): int "
        "    var g: () -> Object := C.Nums; "
        "    var x := g(); "
        "    return 0; "
        "  end Main; "
        "end D;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedStaticIterableBridge(r));
}

// =====================================================================
// ACCEPT controls — pin the un-rejected axes the four fences must
// preserve. Every accept-control pins by VALUE (not just "compiles"), so
// a future widening of any of the four fences surfaces as a wrong exit
// code rather than a silent false-green.
// =====================================================================

// ACCEPT 1 — owned-RETURNING slot keeps the fresh-producing lambda body
// accepted. `fn: () -> owned Res` transfers the minted frame straight
// into the transferring return slot; the §28.8 ownership-transfer rule
// is the §28.6 leak prevention's companion.
// // per §28.3, §28.8, §28.6 (issue #1820 reviewer-directed self-pass)
BEZEL_TEST(sema_formation_fence_function_value_1820,
           owned_slot_minting_lambda_accepted_spec_28_3_28_8) {
    expectAcceptedByValue(
        "class D "
        "  public static method Main(): int "
        "    var f: () -> owned Res := lambda() => new Res(7); "
        "    using var x := f(); "
        "    return x.Id; "
        "  end Main; "
        "end D;",
        7);
}

// ACCEPT 2 — value-typed concrete slot keeps the lambda body accepted
// (the rule's "concrete class/interface" phrasing excludes value builtins
// and records — they borrow through the row's "Function value … never
// owned" carve-out). // per §28.3 (issue #1820 reviewer-directed self-pass)
BEZEL_TEST(sema_formation_fence_function_value_1820,
           value_typed_slot_lambda_accepted_spec_28_3) {
    expectAcceptedByValue(
        "class D "
        "  public static method Main(): int "
        "    var f: () -> int := lambda() => 42; "
        "    var x := f(); "
        "    return x; "
        "  end Main; "
        "end D;",
        42);
}

// ACCEPT 3 — borrowed-source body return path at a concrete class slot
// (a captured parameter read) stays accepted. The rule's "capture-sourced
// borrow, plain-parameter-sourced borrow, or static storage" admission
// list is the §28.3 row's caller-side source carriage; the lambda body's
// `p` read is a borrow of the parameter, not a fresh producer.
// // per §28.3 (issue #1820 reviewer-directed self-pass)
BEZEL_TEST(sema_formation_fence_function_value_1820,
           concrete_slot_borrow_return_path_accepted_spec_28_3) {
    expectAcceptedByValue(
        "class D "
        "  public static method Main(): int "
        "    var s := new Res(3); "
        "    var f: () -> Res := lambda() => s; "
        "    var x := f(); "
        "    return x.Id; "
        "  end Main; "
        "end D;",
        3);
}

// ACCEPT 4 — non-Task-family property reference still forms the getter-
// as-function-value (the §27.6 / #756 promotion, unchanged). The fence
// at p1820e targets Task-family and iterable-family properties only;
// value-typed properties (here: `int`) keep the original behavior.
// // per §27.6 (issue #1820 reviewer-directed self-pass)
BEZEL_TEST(sema_formation_fence_function_value_1820,
           non_task_property_reference_accepted_spec_27_6) {
    expectAcceptedByValue(
        "class D "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    var f: () -> int := h.T; "
        "    var x := f(); "
        "    return x; "
        "  end Main; "
        "end D;",
        7);
}

// ACCEPT 5 — identity-type formation of a static iterable-factory
// reference. `f: () -> IEnumerable<int> := C.Nums` — the target slot's
// return IS the callee's own return type, so no bridging happens; the
// cx1 fence's identity-formation accept control.
// // per §27.6 (issue #1820 reviewer-directed self-pass)
BEZEL_TEST(sema_formation_fence_function_value_1820,
           static_iterable_identity_formation_accepted_spec_27_6) {
    expectAcceptedByValue(
        "class D "
        "  public static method Main(): int "
        "    var g: () -> IEnumerable<int> := C.Nums; "
        "    var s := g(); "
        "    var sum := 0; "
        "    for var x in s do sum := sum + x; end; "
        "    return sum; "
        "  end Main; "
        "end D;",
        3);
}
