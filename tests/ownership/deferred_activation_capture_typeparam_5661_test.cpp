// Ownership sema — issue #5661 (§28.3 *Argument contributions* clause 4,
// v0.326; §28.1 clauses 3/5, v0.326; §14.2): a deferred-activation frame
// returned from a GENERIC method carries borrows of the scope-bound owned
// local its plain function-typed argument's closure captures — but the
// capture was silently dropped.
//
// The defect, in `MethodOwnership::deferredActivationCaptureSources`
// (Ownership_Checks.cpp), the LocalSymbol arm of the capture reduction:
//
//     if (ls->declType && !isBorrowableType(ls->declType)) continue;
//
// `isBorrowableType` classifies a **bare type parameter** as NOT borrowable —
// a Named TypeRef with a null `declaration`, the shape §31.1's comment shares
// with builtin exceptions and value builtins. So a captured `T`-typed local
// (`using var t := new T()`) was dropped by the value-typed-capture gate and
// the returned frame's carried-source set silently lost its scope-bound borrow:
// the consumer would enumerate into a closure over a disposed object. The
// identical shape in a NON-generic method (`var t := new Item();` captured by
// the same kind of closure) is rejected — the hole is exactly the generic body.
//
// §28.3 *Argument contributions* clause 4 fixes the reading: in a generic body,
// "an argument whose type is a type parameter ... contributes at every
// substitution the union of what each substitution its constraints admit would
// contribute — **at a type parameter admitting a class or interface
// substitution, what a class- or interface-typed argument contributes**."
// §28.1 clauses 3/5 supply the weakest assumption and its one exclusion: the
// generic body type-checks under the class-or-interface assumption, and a
// declaration whose constraints exclude class and interface substitutions
// outright (`where T: value`, §14.2) sits outside the rule entirely. The fix
// adds exactly that one reading, POSITIONALLY (the head must name one of this
// body's own generic parameters, so a 0-arity builtin sharing the
// null-declaration Named shape is not mistaken for one); every other type keeps
// `isBorrowableType`'s answer verbatim.
//
// SCOPE — the class/interface LIMB ONLY. Clause 4 has a second limb, which this
// fix does NOT implement: "under `where T: value`, the sources of the borrows it
// may carry" (a borrow-bearing record / inline array / choice-holding-borrows
// argument, §28.3 clause 3). Here a `where T: value` capture simply contributes
// NOTHING — a narrowing under-approximation of the spec's weakest assumption.
// It is SOUND in the safe direction (it can only admit more, never reject a
// legal program), but it is not "complete". `accept_value_constrained_
// typeparam_local_capture` below pins the behaviour as it stands.
//
// The distinction this fix draws, and the reason it does NOT weaken
// `deferred_activation_capture_1818.traced_local_reassigned_conservative_accepted`:
// a capture whose source is a **scope-bound owned binding** is dropped only when
// its TYPE is not borrow-bearing. A local that is merely REASSIGNED before the
// frame outlives it is not a capture question at all — it is the pre-scan's
// untraceable-provenance gate (`scopedAssignedSyms_`, in the identifier
// provenance row), which runs before and independently of this type gate and is
// untouched. Row 5 below is that same blocker shape in its generic form: it must
// stay ACCEPTED, and it does.

#include <string>

#include "support/ownership_support.hpp"  // parseAndCheckOwnership / hasErrorMessage
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// The frame factory: a plain (non-`owned`) FUNCTION-TYPED parameter whose body
// does the work — it invokes the callback and yields the result, so every
// source the closure carries rides the returned frame (§28.3 K6 "Frames are
// values"; §28.14 *Call-bounded × deferred activation*). `Make<T>` mints the
// argument the callback is applied to, so the body has nothing else to blame.
//
// `WrapV<T>` is the same factory for a VALUE-constrained `T`: it cannot mint a
// `T` (a `where T: value` parameter admits no class substitution, so `new T()`
// is not available), so it takes the value to apply the callback to as a second
// plain parameter — which is itself an ordinary caller-re-established source
// (§28.3 *Argument contributions* clause 1) and not what the row under test
// turns on.
const char* kPreamble =
    "module M; "
    "class Item "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "end Item; "
    "class C "
    "  public static method Make<T>(): owned T where T: new() => new T(); "
    "  public static method WrapG<T>(f: (T) -> T): IEnumerable<T> "
    "    yield return f(C.Make<T>()); "
    "  end WrapG; "
    "  public static method WrapV<T>(f: (T) -> T, v: T): IEnumerable<T> "
    "    yield return f(v); "
    "  end WrapV; ";

// True when the §28.3 capture-set-contribution diagnostic is present — pins THIS
// bug rather than any incidental error (the support header's hasErrorMessage
// note: a bare hasErrors() also passes when an unrelated rule fires).
bool sawCaptureSetError(const bezel::test::OwnershipResult& r) {
    return hasErrorMessage(r, "capture-set contribution");
}

}  // namespace

// ===========================================================================
// REJECT — a captured type-parameter owned local rides an escaping frame
// ===========================================================================

// ★ Primary regression (spec §28.3 *Argument contributions* clause 4, v0.326):
// `t` is a `using var` of the generic parameter `T`, disposed at the end of S's
// scope. The closure `lambda(x: T) => t` is passed to `WrapG`'s plain
// function-typed slot, whose body invokes it and yields the result, so the
// returned `IEnumerable<T>` frame carries a BORROW of `t`. `T` admits a class
// substitution (`where T: new()` does not exclude one), so clause 4 counts the
// capture at what a class- or interface-typed argument contributes. Rejected.
BEZEL_TEST(deferred_activation_capture_typeparam_5661,
           reject_typeparam_owned_local_capture_generic_func_slot) {
    auto r = parseAndCheckOwnership(
        std::string(kPreamble) +
        "  public static method S<T>(): IEnumerable<T> where T: new() "
        "    using var t := new T(); "
        "    return WrapG<T>(lambda(x: T) => t); "
        "  end S; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(sawCaptureSetError(r));
    // The diagnostic must name `t` as the escaping borrow source.
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "captures the owned value `t`"));
}

// The AFFINE auto-drop plain local (`var t := new T()`, no `using`) — the same
// read through `localIsOwnedObligation`'s auto-drop row rather than its
// `using var` row. Rejected identically.
// per §28.3 *Argument contributions* clause 4 / *Auto-drop (affine)*, v0.326
BEZEL_TEST(deferred_activation_capture_typeparam_5661,
           reject_typeparam_plain_local_capture_generic_func_slot) {
    auto r = parseAndCheckOwnership(
        std::string(kPreamble) +
        "  public static method S<T>(): IEnumerable<T> where T: new() "
        "    var t := new T(); "
        "    return WrapG<T>(lambda(x: T) => t); "
        "  end S; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(sawCaptureSetError(r));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "captures the owned value `t`"));
}

// The capture reaches the slot through a plain function-valued LOCAL holding
// the lambda (never reassigned) — the T5 traced-local provenance row. The type
// gate sits under that row's reduction, so the same drop, and the same reject.
// per §28.3 *Argument contributions* clause 4 / traced-local row, v0.326
BEZEL_TEST(deferred_activation_capture_typeparam_5661,
           reject_typeparam_capture_through_traced_fn_local) {
    auto r = parseAndCheckOwnership(
        std::string(kPreamble) +
        "  public static method S<T>(): IEnumerable<T> where T: new() "
        "    using var t := new T(); "
        "    var f := lambda(x: T) => t; "
        "    return WrapG<T>(f); "
        "  end S; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(sawCaptureSetError(r));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "captures the owned value `t`"));
}

// The escape position is the STORE, not the return: the frame is bound to `q`
// and `q` returned. §28.3's rule reads the position (bound / returned / stored),
// so it fires identically.
// per §28.3 *Argument contributions* clause 4 + K6 "Frames are values", v0.326
BEZEL_TEST(deferred_activation_capture_typeparam_5661,
           reject_typeparam_capture_store_escape) {
    auto r = parseAndCheckOwnership(
        std::string(kPreamble) +
        "  public static method S<T>(): IEnumerable<T> where T: new() "
        "    using var t := new T(); "
        "    var q := WrapG<T>(lambda(x: T) => t); "
        "    return q; "
        "  end S; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(sawCaptureSetError(r));
}

// ===========================================================================
// ACCEPT — the boundaries this fix must NOT cross
// ===========================================================================

// ★ THE LOad-bearing over-rejection guard, generic form (the blocker shape).
// The function-valued local `f` is REASSIGNED before the frame outlives it, so
// the flow-insensitive pre-scan cannot pin which closure it holds; it is
// untraceable, contributes nothing, and the frame is admitted CONSERVATIVELY —
// even though `f` was once bound to a closure over the owned `using var t`.
// This is `deferred_activation_capture_1818.traced_local_reassigned_conservative_accepted`
// transplanted to a generic body: the reassignment gate
// (`scopedAssignedSyms_`) is the provenance question and is untouched by this
// fix, whose type gate never sees an untraceable local.
// per §28.3 *Argument contributions* clause 4 (contributes at every
// substitution — none of which the analysis can reach), §28.3 traced-local row,
// §28.14, v0.326
BEZEL_TEST(deferred_activation_capture_typeparam_5661,
           accept_reassigned_fn_local_conservative) {
    auto r = parseAndCheckOwnership(
        std::string(kPreamble) +
        "  public static method S<T>(): IEnumerable<T> where T: new() "
        "    using var t := new T(); "
        "    var f := lambda(x: T) => t; "
        "    f := lambda(x: T) => C.Make<T>(); "
        "    return WrapG<T>(f); "
        "  end S; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A PARAMETER-sourced capture: `p` is a plain parameter, which outlives the
// method and which every caller re-establishes against its own actual
// (§28.3's parameter-sourced admission; K2.4 signature-level
// re-establishment). Admitted — guards against the type gate over-rejecting a
// type-parameter capture whose source is not scope-bound.
// per §28.3 *Argument contributions* clause 1 + K2.4, v0.326
BEZEL_TEST(deferred_activation_capture_typeparam_5661,
           accept_param_sourced_capture) {
    auto r = parseAndCheckOwnership(
        std::string(kPreamble) +
        "  public static method S<T>(p: T): IEnumerable<T> where T: new() "
        "    return WrapG<T>(lambda(x: T) => p); "
        "  end S; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An `owned` PARAMETER is a scope-bound obligation this method discharges at
// its own end, so the same capture over it is rejected — the capture-check
// counterpart of the parameter arm of the admit row above. Pins that the type
// gate did not accidentally admit every type-parameter capture.
// per §28.3 *Captured owned bindings*, v0.326
BEZEL_TEST(deferred_activation_capture_typeparam_5661,
           reject_typeparam_capture_over_owned_param) {
    auto r = parseAndCheckOwnership(
        std::string(kPreamble) +
        "  public static method S<T>(p: owned T): IEnumerable<T> where T: new() "
        "    return WrapG<T>(lambda(x: T) => p); "
        "  end S; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(sawCaptureSetError(r));
}

// The frame is NOT escaped: it is a direct `for` source, whose desugared
// `using var` bounds it by the loop, which the method-scope `t` outlives.
// Sound, admitted — the for-source position is excluded exactly as it is for
// the concrete-type capture rows. Guards against the type gate widening the
// escape set past the position gate.
// per §28.3 *Argument contributions* clause 4 + §29.2 (the desugared `using
// var`), §28.14, v0.326
BEZEL_TEST(deferred_activation_capture_typeparam_5661,
           accept_for_source_not_escaping) {
    auto r = parseAndCheckOwnership(
        std::string(kPreamble) +
        "  public static method S<T>(): int where T: new() "
        "    using var t := new T(); "
        "    var total := 0; "
        "    for var v in WrapG<T>(lambda(x: T) => t) do "
        "      total := total + 1; "
        "    end; "
        "    return total; "
        "  end S; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A `where T: value` PARAMETER capture is admitted — but NOT through the
// clause-5 exclusion, and this row does NOT pin that exclusion.
//
// WHY IT DOESN'T PIN IT: `bump` is a parameter, and the capture reduction's
// ParameterSymbol arm pushes `{id, sym, symbolIsOwnedObligation(sym)}` and
// `continue`s — it structurally CANNOT reach the LocalSymbol arm's
// `captureTypeIsBorrowBearing(ls->declType)` gate, which is the only call site
// of `captureParamIsValueConstrained`. The verdict below is decided by the
// parameter arm (a plain, non-`owned` parameter outlives the method and the
// caller re-establishes it, §28.3 *Argument contributions* clause 1 / K2.4).
// MEASURED: disabling `captureParamIsValueConstrained` (making it `return false`
// outright) leaves this row PASSING. The row that actually discriminates the
// exclusion is `accept_value_constrained_typeparam_local_capture` below.
//
// Kept as a stable control on the parameter arm under a value constraint: the
// value exclusion must not change the parameter arm's answer either way.
// per §28.1 clause 5, §14.2 `where T: value`, §28.3 *Argument contributions*
// clause 1, v0.326
BEZEL_TEST(deferred_activation_capture_typeparam_5661,
           accept_value_constrained_param_capture_via_parameter_arm) {
    auto r = parseAndCheckOwnership(
        std::string(kPreamble) +
        "  public static method S<T>(bump: T): IEnumerable<T> where T: value "
        "    return WrapG<T>(lambda(x: T) => bump); "
        "  end S; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ★ The §28.1 clause 5 / §14.2 EXCLUSION — the row that actually pins it.
//
// `local` is a `using var` whose DECLARED TYPE IS THE BARE TYPE PARAMETER `T`,
// so the capture reduction classifies it in the LocalSymbol arm and the type
// gate `captureTypeIsBorrowBearing(ls->declType)` runs on it — the only path
// to `captureParamIsValueConstrained`. `where T: value` excludes `T` from class
// and interface substitutions outright (§28.1 clause 5, §14.2), so §28.3
// *Argument contributions* clause 4's "at a type parameter admitting a class or
// interface substitution" never applies: the capture contributes nothing and
// the frame is admitted.
//
// MEASURED NON-VACUITY: with `captureParamIsValueConstrained` disabled (forced
// to `return false`), this program IS rejected with the §28.3
// capture-set-contribution error naming `local`. With the exclusion intact it is
// admitted. The row therefore discriminates the clause-5 test.
//
// NOTE this is admitted WITHOUT error even though `local` is a `using var` —
// under `where T: value` the value is copied into the capture cell, so there is
// no borrow of a scope-bound source for the frame to carry.
// per §28.1 clause 5, §14.2 `where T: value`, §28.3 *Argument contributions*
// clause 4, v0.326
BEZEL_TEST(deferred_activation_capture_typeparam_5661,
           accept_value_constrained_typeparam_local_capture) {
    auto r = parseAndCheckOwnership(
        std::string(kPreamble) +
        "  public static method S<T>(seed: T): IEnumerable<T> where T: value "
        "    using var local := seed; "
        "    return WrapV<T>(lambda(x: T) => local, seed); "
        "  end S; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The CONCRETE-TYPE control: the identical capture over a non-generic owned
// local, which the pre-existing T2 row already rejected. Pins that the fix
// changed nothing for a non-generic body — the `captureTypeIsBorrowBearing`
// gate must return `isBorrowableType`'s own answer there.
// per §28.3 *Argument contributions* clause 1, §28.14, v0.326
BEZEL_TEST(deferred_activation_capture_typeparam_5661,
           reject_concrete_type_control_unchanged) {
    auto r = parseAndCheckOwnership(
        std::string(kPreamble) +
        "  public static method S(): IEnumerable<Item> "
        "    using var t := new Item(); "
        "    return WrapG<Item>(lambda(x: Item) => t); "
        "  end S; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(sawCaptureSetError(r));
}