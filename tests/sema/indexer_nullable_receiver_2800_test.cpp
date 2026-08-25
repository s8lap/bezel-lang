// Spec-derived regression test for issue #2800.
//
// §7.4 (Member access on nullable values) / §23.4 (Indexers), v0.240.
//
// §7.4 is verbatim: "Member access, method calls, and **indexing** on a
// nullable value are compile errors unless the compiler has narrowed it to
// non-null." The "indexing" half was unenforced — the IndexExpr typing block
// in ExprTypecheck_Exprs.cpp read `ctx_.exprType(*ix->target)` and never
// consulted `tt->nullable`, so a nullable receiver typed exactly like its
// non-nullable form and the program compiled. A separate hole (Hole 2 in the
// issue): the IndexExpr arm rejected "no indexer overload matches the N
// arguments" but did NOT reject "the receiver declares NO indexer at all" —
// `!anyIndexer` fell through with no diagnostic, leaving the IndexExpr
// untyped and codegen degrading to a zero value behind an "internal:" note
// (#2434).
//
// FIX #2800 adds (a) a §7.4 nullable-receiver gate at the start of the
// `IndexExpr` arm (a SIMPLE VARIABLE IdentifierExpr is checked via
// `isKnownNonNull`; everything else is never narrowable — TOCTOU, copy to
// local) and (b) a `!anyIndexer` else-branch in the user-defined-indexer
// resolution path that emits a sourced §23.4 "no indexer" diagnostic
// instead of falling through to the silent-zero codegen path.
//
// Both halves are pinned: every negative case asserts on the diagnostic TEXT
// (NOT the coarse rejects() helper, which cannot tell a genuine compile-time
// rejection from a runtime SIGSEGV masquerading — runAot's WIFSIGNALED path
// synthesizes an Error diagnostic for a crash too), and the positive controls
// are the load-bearing half — a widened rejection that breaks working code is
// worse than the bug it fixes, so every receiver shape that §7.4 NARROWING
// (§7.4 "narrowing applies to locals only") or a USER-DECLARED INDEXER keeps
// accepted is pinned here.

#include "support/aot_support.hpp"  // runAot — end-to-end accept control
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

using bezel::test::runAot;

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some reported diagnostic contains `sub`.
bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The §7.4 "indexing on a nullable" diagnostic fragment — names the rule
// verbatim so a sibling site's text (e.g. the §7 index-ARGUMENT gate
// #2619's "is nullable but the target type is non-nullable") cannot
// satisfy it by accident. The shared trailing phrase appears in BOTH the
// SIMPLE-VAR gate (`indexing on nullable `a` requires narrowing it to
// non-null first …`) and the TOCTOU/non-narrowable fallback
// (`indexing on a nullable value requires narrowing it to non-null first
// …`).
constexpr const char* kNullableReceiverDiag =
    "requires narrowing it to non-null first";

// §23.4 "no indexer" diagnostic fragment — distinguishes this defect
// from #2438's "does not implement interface indexer" wording and from
// the "no indexer overload matches the N arguments" arity-mismatch text.
constexpr const char* kNoIndexerDiag =
    "does not declare an indexer";

constexpr const char* kSec74 = "(\xC2\xA7""7.4)";
constexpr const char* kSec234 = "(\xC2\xA7""23.4)";

// A compilation unit wrapping `body` in a class C. The class also declares
// a couple of helper types so the no-indexer case (Hole 2) can use a
// realistic user class without dragging in cross-file declarations.
std::string prog(const std::string& body) {
    return std::string("module M; "
                       "class Box "
                       "  public property [i: int]: int => i * 2; "
                       "end Box; "
                       "class NoIdx public _x: int := 7; end NoIdx; "
                       "class C ") + body + " end C;";
}

}  // namespace

// ============================================================================
// HOLE 1 — §7.4 NULLABLE RECEIVER: `a[i]` on an unnarrowed nullable `a`.
// ============================================================================

// #2800 hole 1 — §7.4 / stdlib §4.4 (`Array<T>`: `public property [i: int]: T`).
// The issue's exact repro: `a[0]` on `a: Array<int>?` compiled clean (exit 0,
// no diagnostic) and the program faulted at runtime with
// `IndexOutOfRangeException` — §7.4 names "indexing" verbatim alongside
// member access and method calls.
BEZEL_TEST(sema_indexer_nullable_receiver_2800,
           array_nullable_receiver_rejected_7_4) {
    auto r = parseSemaAll(prog(
        "public static method Main(): int "
        "  var a: Array<int>? := null; "
        "  var w := a[0]; "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableReceiverDiag));
    BEZEL_EXPECT_TRUE(hasError(r, kSec74));
}

// #2800 hole 1 — §7.4 / stdlib §4.6 (`string` indexer). §7.4 covers
// indexing on a nullable string, exactly as it does for any other
// receiver. Confirms the gate is receiver-shape-agnostic.
BEZEL_TEST(sema_indexer_nullable_receiver_2800,
           string_nullable_receiver_rejected_7_4) {
    auto r = parseSemaAll(prog(
        "public static method Main(): int "
        "  var s: string? := null; "
        "  var c := s[0]; "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableReceiverDiag));
    BEZEL_EXPECT_TRUE(hasError(r, kSec74));
}

// #2800 hole 1 — §7.4 / stdlib §4.8 (`Dictionary<K,V>`). The receiver type
// carries genericArgs[1] (the V element type), so the IndexExpr typing
// branch is the §4.8 key-indexed one — confirms the §7.4 gate fires
// BEFORE that branch's `setExprType(&e, ...)` is reached, not silently
// dropped.
BEZEL_TEST(sema_indexer_nullable_receiver_2800,
           dictionary_nullable_receiver_rejected_7_4) {
    auto r = parseSemaAll(prog(
        "public static method Main(): int "
        "  var d: Dictionary<int, int>? := null; "
        "  var v := d[0]; "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableReceiverDiag));
    BEZEL_EXPECT_TRUE(hasError(r, kSec74));
}

// #2800 hole 1 — §7.4 / §23.4 USER-DECLARED INDEXER. A nullable
// receiver of a user-declared class with an indexer — same §7.4 rule
// applies, just routed through the user-defined-indexer branch instead
// of the builtin-collection one.
BEZEL_TEST(sema_indexer_nullable_receiver_2800,
           user_indexer_nullable_receiver_rejected_7_4) {
    auto r = parseSemaAll(prog(
        "public static method Main(): int "
        "  var b: Box? := null; "
        "  var w := b[0]; "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(hasError(r, kNullableReceiverDiag));
    BEZEL_EXPECT_TRUE(hasError(r, kSec74));
}

// #2800 hole 1 — §7.4 NARROWING CARVE-OUT (positive control, §7.4
// "narrowing applies to locals only"). A nullable local AFTER an `if x <>
// null` guard is provably non-null on the then-arm; the IndexExpr is
// admitted. A naive "tt->nullable → reject" would break this; pinning
// the §7.4 carve-out keeps the fix narrow. The Array<T> is initialized to
// a non-null array carrying one element, so a[0] actually returns 42 at
// runtime — the test verifies BOTH the compile-time accept and the
// runtime value.
BEZEL_TEST(sema_indexer_nullable_receiver_2800,
           array_nullable_receiver_narrowed_admitted_7_4) {
    auto r = bezel::test::runAot(prog(
        "public static method Main(): int "
        "  var a: Array<int>? := null; "
        "  using var tmp := new Array<int>(); "
        "  tmp.Add(42); "
        "  a := tmp; "
        "  if a <> null then "
        "    return a[0]; "  // narrowed — non-null on then-arm
        "  end if; "
        "  return -1; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// #2800 hole 1 — POSTFIX-BANG CARVE-OUT (positive control, §7.8).
// `a![0]` is `PostfixBangExpr(IndexExpr)`; the PostfixBangExpr's
// exprType is the IndexExpr's type, but with `nullable = false` —
// a hard force-unwrap on a known-null receiver still faults at runtime
// (§7.8), but the compile-time type is non-nullable, so the §7.4 gate
// on the receiver does NOT fire (the bang spells the user's
// acknowledgment of the null risk). Pinning this prevents a
// over-rejection that would block the entire `a![0]` family.
BEZEL_TEST(sema_indexer_nullable_receiver_2800,
           array_nullable_receiver_force_unwrap_admitted_7_8) {
    auto r = bezel::test::runAot(prog(
        "public static method Main(): int "
        "  var a: Array<int>? := null; "
        "  return a![0]; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.ran);
    // The bang asserts non-null; runtime null-deref raises
    // NullReferenceException and the process exits with a non-zero
    // status — but the COMPILE is clean (no sema diagnostic), which is
    // the §7.8 carve-out being verified.
    (void)r.exitCode;
}

// ============================================================================
// HOLE 2 — §23.4 NO INDEXER AT ALL: receiver type declares no indexer.
// ============================================================================

// #2800 hole 2 — the issue's exact repro: `n[0]` on `n: NoIdx` where
// `NoIdx` declares no indexer at all. Pre-fix: exit 0 with only an
// "internal: lowerLocalDecl's initializer expression failed to lower
// to a value ... — #2434" note, the local silently zero-initialized.
// Post-fix: a sourced §23.4 "no indexer" error.
BEZEL_TEST(sema_indexer_nullable_receiver_2800,
           no_indexer_at_all_rejected_23_4) {
    auto r = parseSemaAll(prog(
        "public static method Main(): int "
        "  using var n := new NoIdx(); "
        "  var v := n[0]; "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(hasError(r, kNoIndexerDiag));
    BEZEL_EXPECT_TRUE(hasError(r, kSec234));
}

// #2800 hole 2 — a RECORD receiver with no indexer. The user-defined-
// indexer branch (`tt->kind == TypeRef::Kind::Named && tt->declaration`)
// matches both class and record declarations; the `!anyIndexer` else
// branch must fire on both shapes, not just classes.
BEZEL_TEST(sema_indexer_nullable_receiver_2800,
           record_no_indexer_at_all_rejected_23_4) {
    auto r = parseSemaAll(
        std::string("module M; "
                    "record NoIdxRec public x: int; end NoIdxRec; "
                    "class C "
                    "  public static method Main(): int "
                    "    using var n := new NoIdxRec(); "
                    "    var v := n[0]; "
                    "    return 0; "
                    "  end Main; "
                    "end C;"));
    BEZEL_EXPECT_TRUE(hasError(r, kNoIndexerDiag));
    BEZEL_EXPECT_TRUE(hasError(r, kSec234));
}

// #2800 hole 2 — ACCEPT control: a user-declared indexer on the same
// kind of class (a class with one declared indexer, not none) must
// continue to compile and run with the correct value. Pinning this
// ensures the §23.4 "no indexer" reject does not over-fire on the
// ordinary indexer case.
BEZEL_TEST(sema_indexer_nullable_receiver_2800,
           declared_indexer_accepted_runs_correct_value_23_4) {
    auto r = runAot(prog(
        "public static method Main(): int "
        "  using var b := new Box(); "
        "  return b[5]; "  // Box declares [i: int]: int => i*2 → 10
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// #2800 hole 2 — ARITY-MISMATCH CARVE-OUT (positive control, §23.4).
// The issue notes that #2619's §7 index-argument gate now fires on
// these receivers too, and that adding the §23.4 "no indexer"
// diagnostic must not displace it. A receiver with ONE declared
// indexer but TWO index arguments is a §23.4 ARITY mismatch — the
// existing `no indexer overload matches the N arguments` branch
// must keep firing on it, NOT the new `!anyIndexer` branch.
BEZEL_TEST(sema_indexer_nullable_receiver_2800,
           arity_mismatch_keeps_existing_diagnostic_23_4) {
    auto r = parseSemaAll(prog(
        "public static method Main(): int "
        "  using var b := new Box(); "
        "  return b[0, 0]; "
        "end Main;"));
    // §23.4: "no indexer overload matches the 2 arguments" — the
    // existing arity-mismatch diagnostic, NOT the new
    // `does not declare an indexer` message (which would imply the
    // receiver has no indexer at all).
    BEZEL_EXPECT_TRUE(hasError(r, "no indexer overload matches"));
    BEZEL_EXPECT_FALSE(hasError(r, kNoIndexerDiag));
}
