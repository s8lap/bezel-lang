// Ownership — `MethodOwnership::producerState`'s `move` classifier (used by
// `constInitializerRejected`, §21.1's authority) required the moved-from
// operand's TypeRef to carry a user `TypeSymbol` (`opTy->declaration`)
// before classifying `move x` as an owned production. A DECLARATION-LESS
// builtin reference type — `StringBuilder`, `Random`, `Mutex`, `Array<T>`,
// … (no user `TypeDecl`, per TypeResolution_Names.cpp's nullary-builtin
// registration) — therefore fell through to this function's Borrowed
// default, and `const y := move x;` compiled with ZERO diagnostics for one
// of these, even though the byte-identical shape over a USER-declared class
// (trivial-dispose OR non-trivial-dispose — the arm above never gated on
// dispose triviality at all) was already correctly rejected. Issue #3709.
//
// Spec, v0.298:
//   §21.1 "Variable declarations", the runtime-const "Ownership" paragraph,
//     verbatim: "Rejected, with a diagnostic naming this rule and the
//     `using var` remedy: a fresh `new`, an `owned`-returning call, a
//     `Task`-family or iterable-family call (...), a `move` (the marked
//     transfer `const y := move x;` included — not a form), and a
//     move-only value type."  No carve-out for a builtin type or a
//     trivial-dispose verdict.
//   §28.3 "Local variables, ownership, and borrows", the *Producers* table,
//     verbatim row: "`new T(...)` | Owned" — unconditional; no builtin or
//     trivial-dispose exception appears anywhere in that table.
//   §28.3 "Move semantics for owned locals", verbatim: "A fresh declaration
//     (`var y := move x;`, `using var y := move x;`), an **uninitialized**
//     LHS (...), or a **polymorphic-null** LHS (...) adopts owned state
//     from the marked transfer, exactly as it would from a fresh producer"
//     — i.e. `move x`'s classification must match what `new` of `x`'s type
//     would produce. The `NewExpr` arm of `producerState` (same function,
//     above this one in Ownership.cpp) already answers that question for a
//     declaration-less builtin by consulting
//     `common::isHeapPointerReferenceBuiltin` (a heap-pointer reference
//     builtin classifies Owned; a value-shaped one — `DateTime`, `Guid`, …
//     — classifies Borrowed); the `move` arm had no equivalent branch.
//
// THE FIX: `producerState`'s `move` arm gained a second branch, gated on
// `!opTy->declaration` (mirroring the `NewExpr` arm's own declaration-less
// branch), consulting the SAME `common::isHeapPointerReferenceBuiltin`
// authority. A bare, unresolved generic type parameter is ALSO a
// declaration-less `Named` TypeRef, but its head name never is a member of
// the fixed `kHeapPointerReferenceBuiltins` table, so it is unaffected.
//
// narrow #3709 / #4191 / #4196 (review finding, v0.298):
// `kHeapPointerReferenceBuiltins` is a CODEGEN-REPRESENTATION table ("is
// this declaration-less builtin's LLVM lowering a pointer"), not an
// OWNERSHIP authority, and the two questions diverge for
// `CancellationToken` (stdlib §6.1): a `public record` — "a freely-
// copyable record" that is "a borrow-bearing record whose borrow source
// is the source" — with no `Dispose()` and no disposal obligation, yet
// pointer-represented (its record wraps a reference to shared
// cancellation-source state) and so present in the representation table.
// The naive first landing of the fix above used that table alone and so
// classified `move tok` (a `CancellationToken` local) Owned — a NEW
// false-reject: `const t2 := move tok;`, which compiled clean before
// #3709, hard-rejected after it (issue #4191). The `move` arm now ALSO
// excludes any name in the new `sema::isNoDisposalObligationBuiltinName`
// predicate's set (the authoritative "no disposal obligation when
// directly held" predicate, SemaContext.hpp — a strict subset of
// `isValueBuiltinByName` that EXCLUDES `Buffer` because `Buffer<T>` DOES
// carry a disposal obligation: `const b3 := move b2;` over a `new
// Buffer<int>(1000)` LEAKS the buffer at scope end, measured by ASan as
// 4016 bytes per allocation, issue #4196) — an EXCLUSION on top of the
// representation table, not a replacement: `isHeapPointerReferenceBuiltin`
// is still required to establish reference-SHAPE in the first place, so
// a bare generic type parameter (also declaration-less Named, never a
// member of either fixed table) stays unaffected either way. The
// `Stopwatch` control below proves the guard is narrow (excludes
// CancellationToken's borrow-bearing record half, leaves Stopwatch's
// genuine trivial-dispose class as Owned exactly like StringBuilder).
//
// per §21.1, §28.3, v0.298, #3709, #4191, #4196

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::hasErrorMessage;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

bool diagContains(const OwnershipResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// The exact §21.1 message text `constInitializerRejected`'s caller emits
// (Ownership.cpp's `LocalConstDecl` arm, the generic Owned-production
// branch) — a stable fragment naming the rule, not the whole sentence.
constexpr const char* kConstOwnedProductionMsg =
    "must not take an owned production";

// `binding` is substituted with either `const y := move v;` or
// `var y := move v;` / a non-`move` control.
// §2.3 / #5034: Stopwatch lives in System.Diagnostics; adding the import here
// unconditionally keeps the helper usable for any builtin without the caller
// needing to know which module each builtin belongs to.
std::string builtinProg(const std::string& newExpr, const std::string& binding) {
    return "module M; import System.Diagnostics; class C "
           "  public static method Main(): int "
           "    var v := " +
           newExpr + "; " + binding +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// =============================================================================
// NEGATIVE (THE BUG) — `const y := move sb;` where `sb: StringBuilder`, a
// declaration-less TRIVIAL-dispose builtin class (stdlib §3.1; no user
// TypeDecl). FAILS WITHOUT THE FIX: compiles with zero diagnostics.
// =============================================================================
BEZEL_TEST(const_move_declless_builtin_3709,
           move_stringbuilder_into_const_rejected) {
    auto r = parseAndCheckOwnership(
        builtinProg("new StringBuilder()", "const y := move v;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kConstOwnedProductionMsg));
}

// =============================================================================
// NEGATIVE (SAME ROOT CAUSE, WIDER) — `const y := move arr;` where
// `arr: Array<int>`, a declaration-less TRIVIAL-dispose BORROWING-collection
// builtin (§28.9's "trivial-dispose for generic class instantiations").
// FAILS WITHOUT THE FIX: compiles with zero diagnostics.
// =============================================================================
BEZEL_TEST(const_move_declless_builtin_3709,
           move_array_into_const_rejected) {
    auto r = parseAndCheckOwnership(
        builtinProg("new Array<int>()", "const y := move v;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kConstOwnedProductionMsg));
}

// =============================================================================
// NEGATIVE (SAME ROOT CAUSE, WIDER) — `const y := move m;` where `m: Mutex`,
// a declaration-less NON-TRIVIAL-dispose builtin (stdlib §6.2). Proves the
// bug was never actually keyed on dispose triviality — EVERY
// declaration-less builtin reference type fell through, trivial or not.
// FAILS WITHOUT THE FIX: compiles with zero diagnostics.
// =============================================================================
BEZEL_TEST(const_move_declless_builtin_3709,
           move_mutex_into_const_rejected) {
    auto r =
        parseAndCheckOwnership(builtinProg("new Mutex()", "const y := move v;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kConstOwnedProductionMsg));
}

// =============================================================================
// CONTROL — a USER-declared TRIVIAL-dispose class (has a `TypeSymbol`
// declaration, no destructor, no `owned` field) was ALREADY correctly
// rejected before this fix (the pre-fix arm gated on `opTy->declaration`
// being non-null, not on dispose triviality) and must stay rejected with
// the IDENTICAL message, unchanged by this fix.
// =============================================================================
BEZEL_TEST(const_move_declless_builtin_3709,
           move_user_trivial_dispose_class_into_const_rejected_control) {
    auto r = parseAndCheckOwnership(
        "module M; class Sb "
        "  public constructor() end constructor; "
        "end Sb; "
        "class C "
        "  public static method Main(): int "
        "    var v := new Sb(); "
        "    const y := move v; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kConstOwnedProductionMsg));
}

// =============================================================================
// CONTROL — a USER-declared NON-TRIVIAL-dispose class (a destructor) was
// already correctly rejected and must remain so, message unchanged.
// =============================================================================
BEZEL_TEST(const_move_declless_builtin_3709,
           move_user_nontrivial_dispose_class_into_const_rejected_control) {
    auto r = parseAndCheckOwnership(
        "module M; class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(): int "
        "    var v := new Res(); "
        "    const y := move v; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kConstOwnedProductionMsg));
}

// =============================================================================
// ACCEPT CONTROL — an ordinary const BORROW of a builtin class (no `move`)
// must stay completely clean: the fix must not turn every const-of-builtin
// initializer into a false rejection.
// =============================================================================
BEZEL_TEST(const_move_declless_builtin_3709,
           const_borrow_of_stringbuilder_accepted_control) {
    auto r = parseAndCheckOwnership(
        builtinProg("new StringBuilder()", "const y := v;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// =============================================================================
// ACCEPT CONTROL — `var y := move sb;` (not `const`) stays accepted: the
// `move` now correctly classifies Owned (this fix), so `y` carries a
// disposal obligation like any other owned local — proven by the standard
// §28.3 auto-drop warning appearing on an undischarged `y`, and by a clean
// compile once `y` is discharged (`_ := y;`).
// =============================================================================
BEZEL_TEST(const_move_declless_builtin_3709,
           var_move_of_stringbuilder_undischarged_autodrop_warns_control) {
    auto r = parseAndCheckOwnership(
        builtinProg("new StringBuilder()", "var y := move v;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "y"));
}

BEZEL_TEST(const_move_declless_builtin_3709,
           var_move_of_stringbuilder_discharged_accepted_control) {
    auto r = parseAndCheckOwnership(
        builtinProg("new StringBuilder()", "var y := move v; _ := y;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "y"));
}

// =============================================================================
// ACCEPT CONTROL (NO NEW FALSE-REJECT) — `move` of a BARE, unconstrained
// generic type parameter is ALSO a declaration-less `Named` TypeRef, but its
// head name (`T`) is never a member of `kHeapPointerReferenceBuiltins`, so
// the new branch must not fire for it — proves the fix does not widen past
// the fixed builtin-name table into every declaration-less type.
// =============================================================================
BEZEL_TEST(const_move_declless_builtin_3709,
           move_bare_generic_parameter_accepted_control) {
    auto r = parseAndCheckOwnership(
        "module M; class Box<T> "
        "  public static method Try(x: T): T "
        "    var t := x; "
        "    var y := move t; "
        "    return y; "
        "  end Try; "
        "end Box;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(diagContains(r, "owned production"));
}

// =============================================================================
// narrow #3709 / #4191 — the review finding's own repro. `CancellationToken`
// (stdlib §6.1) is a `public record`, pointer-represented but carrying NO
// disposal obligation (borrow-bearing, freely copyable, no `Dispose()`). It
// is present in BOTH `kHeapPointerReferenceBuiltins` (representation) and
// `sema::isNoDisposalObligationBuiltinName` (ownership) — the new guard
// must exclude it. FAILS WITHOUT THE FIX (#3709 alone): hard-rejects with
// the owned-production message (measured on the pre-narrow-fix landing).
// =============================================================================
namespace {
std::string cancellationTokenProg(const std::string& binding) {
    return "module M; class C "
           "  public static method Main(): int "
           "    var cts := new CancellationTokenSource(); "
           "    var tok: CancellationToken := cts.Token; " +
           binding +
           "    cts.Dispose(); "
           "    return 0; "
           "  end Main; "
           "end C;";
}
}  // namespace

BEZEL_TEST(const_move_declless_builtin_3709,
           move_cancellationtoken_into_const_accepted_narrow_4191) {
    auto r = parseAndCheckOwnership(
        cancellationTokenProg("const t2 := move tok; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(diagContains(r, "owned production"));
}

// `var y := move tok;` must draw NO §28.3 auto-drop warning either — a
// CancellationToken carries no disposal obligation to discharge, so `t2`/`y`
// must not be tracked as an owned local at all (not merely "rejected as
// const" but genuinely Borrowed, matching a plain value/borrow binding).
BEZEL_TEST(const_move_declless_builtin_3709,
           var_move_of_cancellationtoken_no_autodrop_warning_narrow_4191) {
    auto r = parseAndCheckOwnership(
        cancellationTokenProg("var t2 := move tok; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "t2"));
    BEZEL_EXPECT_FALSE(diagContains(r, "owned production"));
}

// =============================================================================
// narrow #3709 / #4196 — the SECOND review finding. `Buffer<T>` (the #545
// arity-1 compiler-intrinsic primitive) is a heap-allocated value with a
// REAL disposal obligation (its backing buffer must be freed at scope
// end). It is in BOTH `kHeapPointerReferenceBuiltins` (representation)
// AND `sema::isValueBuiltinByName` (the wider owning-collection-element-
// type predicate) — but it MUST stay classified Owned under `move` so the
// `const b3 := move b2;` outright-rejection fires, exactly like the
// StringBuilder reject-control above. Measured against an attempted fix
// that consulted `isValueBuiltinByName` (fed168b): the guard wrongly
// excluded `Buffer`, the rejection was silently converted to an
// accept, and the Buffer allocation LEAKED 4016 bytes at program exit
// (verified under `--sanitize=address`). The new
// `isNoDisposalObligationBuiltinName` predicate explicitly EXCLUDES
// `Buffer`, so this rejection stays loud.
// =============================================================================
BEZEL_TEST(const_move_declless_builtin_3709,
           move_buffer_into_const_rejected_narrow_4196) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Main(): int "
        "    var b2 := new Buffer<int>(4); "
        "    const b3 := move b2; "
        "    _ := b3; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kConstOwnedProductionMsg));
}

// =============================================================================
// CONTROL — `Stopwatch` is ALSO a declaration-less heap-pointer-represented
// builtin (stdlib §8.1, `kHeapPointerReferenceBuiltins`) and is paired with
// `CancellationToken` in the unrelated `kNoOverrideIdentityBuiltins` table,
// but — unlike `CancellationToken` — is NOT in
// `isNoDisposalObligationBuiltinName`'s set: it is a genuine trivial-dispose
// CLASS with real (if compiler-freed, no-op `Dispose`) instance identity,
// not a value/record. `move` of a Stopwatch local into a `const` must STILL
// be rejected — proves the new `!isNoDisposalObligationBuiltinName` guard
// is narrow (excludes exactly the value-shaped overlap) and does not
// disable the #3709 fix for every heap-pointer builtin.
// =============================================================================
BEZEL_TEST(const_move_declless_builtin_3709,
           move_stopwatch_into_const_rejected_control) {
    auto r = parseAndCheckOwnership(
        builtinProg("new Stopwatch()", "const s2 := move v;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kConstOwnedProductionMsg));
}
