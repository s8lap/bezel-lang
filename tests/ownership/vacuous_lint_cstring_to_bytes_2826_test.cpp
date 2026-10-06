// Ownership / diagnostics — the §28.10 vacuous-`using var` lint over-fired on
// `c.ToBytes()` where `c: CString`, even though stdlib §10.1 declares
// `CString.ToBytes(): owned Array<byte>` — the runtime `bz_cstring_to_bytes`
// returns a FRESH, independently-owned `bz_array_t*` (a copy of the CString
// bytes), so the `using var` carries a real §28.10 disposal obligation and
// suppressing it (as the diagnostic advised, `const b := …`) would LEAK the
// array. Issue #2826.
//
// Spec, v0.240:
//   §28.9 "Runtime-managed types" — every type carries a disposal obligation
//     in one of two forms: the vtable slot-2 `Dispose()` or the §28.9
//     trivial-dispose reclaim.
//   §28.10 "Borrow-pinning consistency" — a `using var` over a disposable type
//     is the CORRECT canonical pattern, not vacuous; the scope-end disposal
//     does real work.
//   stdlib §10.1 `CString`: "public method ToBytes(): owned Array<byte>;".
//
// THE BUG. `isBuiltinOwnedMaterializerCall` (OwnershipFacts.cpp:391-479), the
// SHARED authority the §28.10 vacuity lint's outer guard
// `!constInitializerRejected(initializer)` consults for an owned-producing
// call (#2709 / #3017 widening), enumerates the stdlib-declared
// `owned`-returning materializers (ToArray / ToSet / ToDictionary / Sorted /
// Reversed / Slice / Split / Encoding.GetBytes) — but NOT ToBytes. The
// `Known gaps` paragraph in #2709's own test (vacuous_lint_materializer_
// remedy_2709_test.cpp:57-63) and the #2749 follow-up item 2 explicitly named
// ToBytes as the missing entry. `c.ToBytes()` therefore fell through to
// Borrowed, the lint fired on a real disposal obligation, and the advice
// (`const b := …`) silently LEAKED the freshly-allocated `bz_array_t*` — the
// exact defect class #2709 was supposed to close for the rest of the family.
//
// The bug fires on BOTH receiver shapes (the parameter shape `c: CString`
// and the local-from-factory shape `using var c := CString.From(…)`) because
// the result's TypeRef — `Array<byte>` (Named, declaration=null, astNode name
// "Array") — is identical regardless of how the receiver's TypeRef was
// constructed; both receiver shapes reach the same sema typing block at
// ExprTypecheck_Calls.cpp:7840 and stamp the same result. The lint's
// classifier then keys off the RESULT, which is the same Array<byte> in
// either case. So one fix covers both.
//
// THE FIX. Add `ToBytes` to the member-name allowlist in
// `isBuiltinOwnedMaterializerCall`, exactly the way #2709 did for ToArray /
// ToSet / ToDictionary / Sorted / Reversed / Slice / Split / GetBytes and
// exactly the way #3017 widened the static-call-only carve-out. The predicate's
// existing narrowness still gates the fix:
//   * Callee symbol must be NULL (the builtin-only gate — sema's builtin
//     member-call typing path lives entirely inside `if (!calleeSym)`,
//     ExprTypecheck_Calls.cpp).
//   * Result head must be Array / Set / Dictionary — CString.ToBytes() returns
//     `Array<byte>` (the stdlib §10.1 declaration, codified at
//     ExprTypecheck_Calls.cpp:7840-7843), and BinarySerializer.ToBytes /
//     JsonSerializer.ToBytes return `OwningBuffer<byte>` (head
//     `OwningBuffer`, EXCLUDED by the head gate — those are already covered
//     by `isDisposableBuiltinHeadName` and the chain's disposal arms
//     separately, #2992).
//
// NOT A DISPOSAL CHANGE. This edit touches only the emission of a warning.
// `autoDropAtScopeEnd` / `registerUsingDisposal` are untouched; the chain's
// collection-family arm (Array) reclaims the result for the parameter and
// the local-from-factory cases identically. The leak goes away because §21.1
// removes the would-be-remedy `const` program (mirroring #2709 / #3017).
//
// KNOWING PARTIAL COVERAGE — recorded deliberately for the residual items
// (`ToCharArray`, `ToByteArray`, `CopyToArray`, `ReadAll*`, `GetFiles`,
// `GetDirectories`). They are the same shape, the same code path, and were
// left out by the original measurement-gated philosophy (#2749 items 1 and
// 3). They are filed separately; this fix covers the case the user
// reproduced mechanically.

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// The inert-RHS ("Case A") vacuity warning, matched on its TEXT — the coarse
// has-a-diagnostic check cannot tell this warning from any other.
bool hasInertRhsWarning(const OwnershipResult& r, const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("is vacuous") == std::string::npos) continue;
        if (d.message.find("RHS is a value-typed/runtime-managed") ==
            std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

// The vacuous-borrow-pin ("Case B") warning — it carries the SAME `const`
// remedy, so the suppression must not merely push Case A's shapes into it.
bool hasVacuousBorrowPinWarning(const OwnershipResult& r,
                                const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("pins a source that already outlives") ==
            std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

// §21.1's rejection in its NEW materializer wording (#2709). The exact
// `const b := c.ToBytes();` program must be REJECTED — that is what keeps
// the leak the would-be-remedy would introduce from happening.
bool hasConstMaterializerError(const OwnershipResult& r,
                               const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("must not take an owned production") ==
            std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

}  // namespace

// =====================================================================
// (1) THE #2826 REPRO — the parameter-receiver case. `c: CString` on a
// parameter. The receiver TypeRef carries `declaration = null` (the
// ANNOTATION shape, TypeResolution_Names.cpp), and the lint's classifier
// reads the call's RESULT type, not the receiver's, so the result-side
// bug fires identically.
//
// FAILS WITHOUT THE FIX on the pre-fix HEAD: the §28.10 vacuity-lint
// fires for `b`, with text "is vacuous — the RHS is a value-typed/
// runtime-managed value with no disposal obligation (§28.9); use
// `const x := …;`", which if followed would silently leak the
// freshly-allocated bz_array_t*.
// =====================================================================
BEZEL_TEST(vacuous_lint_cstring_to_bytes_2826,
           parameter_receiver_no_vacuous_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method N(c: CString): int "
        "    using var b := c.ToBytes(); "
        "    return b.Count; "
        "  end N; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "b"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "b"));
}

// =====================================================================
// (2) THE LOCAL-RECEIVER CASE — `c` is a `using var` local bound to
// `CString.From(…)`. The receiver TypeRef here carries
// `declaration = builtinClassSym("CString")` (the FACTORY shape, stamped in
// ExprTypecheck_Calls.cpp:6820-6826), NOT the annotation shape.
//
// The result TypeRef is byte-identical to (1) — `Array<byte>` (Named,
// declaration=null, astNode name "Array") — so the receiver shape is
// irrelevant to the classifier; one fix covers both.
//
// FAILS WITHOUT THE FIX in the SAME WAY (1) fails — same warning, same
// would-be-leak. The issue text described only (1) but (2) is the same
// defect with a different receiver shape.
// =====================================================================
BEZEL_TEST(vacuous_lint_cstring_to_bytes_2826,
           local_receiver_no_vacuous_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method M(): int "
        "    using var c := CString.From(\"abcd\"); "
        "    using var b := c.ToBytes(); "
        "    return b.Count; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "b"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "b"));
}

// =====================================================================
// (3) §21.1 RECIPROCAL — the `const` form must now be REJECTED. The
// `const b := c.ToBytes();` program was the would-be remedy the lint
// steered authors to; suppressing the warning without rejecting the
// `const` would just trade the spurious warning for the silent leak.
// Routing through `isBuiltinOwnedMaterializerCall` (now that ToBytes is
// in its list) closes both halves at once — exactly the way #2709 /
// #3017 did for the rest of the family.
//
// FAILS WITHOUT THE FIX on the pre-fix HEAD: `const b := c.ToBytes()`
// compiles silently and the `bz_array_t*` leaks at scope end.
// =====================================================================
BEZEL_TEST(vacuous_lint_cstring_to_bytes_2826,
           const_form_rejected_spec_21_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method M(): int "
        "    using var c := CString.From(\"abcd\"); "
        "    const b := c.ToBytes(); "
        "    return b.Count; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasConstMaterializerError(r, "b"));
}

// =====================================================================
// (4) FALSE-REJECT GUARD — a USER class with a method named `ToBytes`
// returning a BORROW of a field is NOT an owned production (mirrors
// vacuous_lint_materializer_remedy_2709_test.cpp's #2709 false-reject
// guard, and the builtin-only gate's own commentary at OwnershipFacts.cpp:
// 427-446). §21.1 admits borrows verbatim ("Admitted: […] and **borrows**
// — the binding is then a borrow carrying the initializer's source-
// liveness constraints"), so the `const` form must COMPILE.
//
// FAILS WITHOUT THE FIX only if the fix accidentally widens to user
// methods (a regression vs. the builtin-only gate). The fix is gated
// by `ctx.exprSymbol(*call.callee) != nullptr` (returning false on
// symbol-bearing callees), so a user method's MethodSymbol excludes
// this case — verified below.
// =====================================================================
BEZEL_TEST(vacuous_lint_cstring_to_bytes_2826,
           user_method_named_tobytes_returning_borrow_admissible_spec_21_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Holder "
        "  private owned _bytes: Array<byte>; "
        "  public constructor() _bytes := new Array<byte>(4); end constructor; "
        "  destructor end destructor; "
        "  public method ToBytes(): Array<byte> return _bytes; end ToBytes; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); "
        "  const b := h.ToBytes(); "
        "  return b.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasConstMaterializerError(r, "b"));
}

// =====================================================================
// (5) FUNCTION-TYPED FIELD named `ToBytes` — the harder false-reject
// guard, mirroring vacuous_lint_materializer_remedy_2709_test.cpp:273-293.
// `resolveMemberAccess` attaches a FieldSymbol (NOT a MethodSymbol) to
// the callee, and the builtin-only gate's
// `ctx.exprSymbol(*call.callee) != nullptr` test excludes FieldSymbol-
// bearing callees too, so the `const` form must COMPILE.
//
// FAILS WITHOUT THE FIX only if the fix accidentally widens the gate
// to symbol-bearing callees (a regression vs. #2709's own commentary at
// OwnershipFacts.cpp:427-446).
// =====================================================================
BEZEL_TEST(vacuous_lint_cstring_to_bytes_2826,
           function_typed_field_named_tobytes_admissible_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cfg "
        "  private owned _bytes: Array<byte>; "
        "  public ToBytes: () -> Array<byte>; "
        "  public constructor() "
        "    _bytes := new Array<byte>(4); "
        "    ToBytes := lambda() => _bytes; "
        "  end constructor; "
        "  destructor end destructor; "
        "end Cfg; "
        "class C public static method Main(): int "
        "  using var cfg := new Cfg(); "
        "  const b := cfg.ToBytes(); "
        "  return b.Count; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasConstMaterializerError(r, "b"));
}

// =====================================================================
// (6) OVER-SUPPRESSION GUARD — the lint must still fire where its
// remedy IS legal. A borrow of a parameter typed at a built-in
// borrowing-collection head (`using var b := arrField;` where arrField
// is an Array<byte> field on a destructor-bearing class) is exactly
// the case the existing #2695 / #2073 lint was designed for.
//
// FAILS WITHOUT THE FIX is N/A — the test was passing before #2826's
// fix landed and continues to pass after, by design (the materializer
// predicate's first disjunct `producerState(e) == Owned` already
// returns true for that shape and the OUTER guard
// `!constInitializerRejected` short-circuits, but the alias-form
// IdentifierExpr RHS bypasses every CallExpr disjunct here).
// =====================================================================
BEZEL_TEST(vacuous_lint_cstring_to_bytes_2826,
           alias_pin_on_array_field_still_warns_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Box "
        "  private owned _arr: Array<byte>; "
        "  public constructor() _arr := new Array<byte>(3); end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); "
        "  using var p := b._arr; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "p"));
}