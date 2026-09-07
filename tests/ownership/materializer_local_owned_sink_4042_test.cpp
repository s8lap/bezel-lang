// Spec-derived tests for issue #4042 — `producerState` never tracks a plain
// `var` local bound DIRECTLY to a builtin owned-returning "materializer" call
// (`ToArray`/`ToSet`/`ToDictionary`/`Sorted`/`Reversed`/`Slice`/`Split`/
// `GetBytes`/`ToBytes`) as `Owned`, so the local's tracked state stays
// `Borrowed`. Once #3993 (`checkOwnedReturnDirectBorrow`) started reading
// tracked local state against a declared `owned` return type, this
// pre-existing gap became a live FALSE-REJECT: `var items := seq.Sorted();
// return items;` (and `return move items;`) was rejected as "a borrow cannot
// be returned as an `owned` value", even though `Sorted()`'s result IS a
// genuine owned production (stdlib §5: "the return is `owned` — the caller
// is responsible for disposing it"). Only the DIRECT spelling
// (`return seq.Sorted();`, no intervening local) compiled.
//
// THE TRAP (why the obvious fix is wrong). The natural-looking fix mirrors
// the existing `IndexExpr` arm `producerState` already has for the
// range-slice `arr[0..2]` (fix #2753): add a `CallExpr` arm consulting
// `common::isBuiltinOwnedMaterializerCall` and return `Owned`. That predicate's
// own header comment (`src/common/OwnershipFacts.hpp`, the
// "WHY THIS IS NOT `producerState`" paragraph) explicitly forbids exactly
// this:
//
//   "producerState is the free-deciding input at handleLocalDecl
//    (Ownership.cpp), and codegen already reclaims these values on its own
//    recognition for both bindable spellings (a plain `var` via
//    `isFreshBorrowingArrayMaterializerLocal`, a `using var` via
//    `registerUsingDisposal`). Reclassifying them `Owned` would register a
//    SECOND free on top of codegen's — the double-free direction."
//
// THE FIX SHIPPED HERE does NOT touch `producerState`/`states_`/
// `handleLocalDecl` at all — the disposal-driving machinery the warning
// above is about is completely unchanged. Instead it narrows
// `MethodOwnership::ownedArgSourceIsBorrow` (Ownership_Checks.cpp), the
// shared, side-effect-free CLASSIFIER every owned-sink CHECK (
// `checkOwnedArgSource`, `checkOwnedReturnDirectBorrow`,
// `checkOwnedReturnBorrowSource`, `checkThrowOperandNotBorrowed`, the owned-
// field-assignment check) consults: when a tracked local's state reads
// `Borrowed` AND its declaration's initializer is DIRECTLY one of these
// builtin materializer calls, the classifier answers "not a borrow" instead
// of trusting the raw tracked state. `ownedArgSourceIsBorrow` has no side
// effects of its own (every call site only feeds a `report(...)`), so this
// change can only ever make MORE programs compile — it cannot alter what
// codegen frees for any already-accepted program.
//
// WHY THIS IS SAFE (verified empirically under ASan/LSan, not by argument —
// see the amplified-loop tests below). Codegen's own escape walk
// (`StringEscapeWalker` / `collectBorrowingArrayFreeEligible`,
// `LlvmBackend_Setup.cpp`) ALREADY treats a `return`/`return move`/owned-
// field-store/owned-argument use of the tracked identifier as an ESCAPE
// (`expr(r->value.get(), /*safeRead=*/false)` for `ReturnStmt`, which sets
// `escaped=true` for a bare identifier read with `safeRead=false`) and
// therefore excludes it from `isFreshBorrowingArrayMaterializerLocal`'s
// scope-end reclaim — exactly the same way it already excludes a returned
// `new`-produced local. Since `producerState`/`states_` are untouched, the
// ownership pass's OWN `everOwnedPlainLocals_`/`autoDropAtScopeEnd` affine
// auto-drop mechanism never even considers this local (it only ever fires
// for a local `producerState` classified `Owned`). So admitting these five
// owned-sink shapes at the CHECK layer does not create a second free path;
// codegen was already correctly excluding the local from its own reclaim
// once it observed the sink use, for the SAME reason it already does so for
// `return newLocal;`.
//
// Conformance: this fix restores `tests/conformance/cases/spec_0175.bzl`
// (spec §26.4 *Generic extensions* — its own `IEnumerable<T>.Sort()` worked
// example) from `blocked-on-#4042` back to `fragment`/`compiles` in
// `tests/conformance/manifest.tsv`.
//
// FAIL-WITHOUT / PASS-WITH, measured on this worktree (base commit
// a6e270d) by reverting `src/ownership/Ownership_Checks.cpp` and rebuilding:
// every REJECT-repro below (materializer_local_owned_sink_4042 suite, the
// non-"reject_control"/"transitive_alias" cases) failed to compile pre-fix
// with the diagnostic text asserted here, and compiled clean post-fix.
//
// per §21.1, §28.3, §28.6, §28.8, stdlib §5 / §4.4 / §9.6, v0.294, #4042

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot / runAotChecked — AOT(+ASan) oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

constexpr const char* kDirectBorrowReturn =
    "a borrow cannot be returned as an `owned` value";
constexpr const char* kMoveOfBorrowReturn =
    "cannot return a borrow as an `owned` value";
constexpr const char* kOwnedFieldStoreBorrow =
    "a borrow cannot be stored into an `owned` field";
constexpr const char* kOwnedArgBorrow =
    "a borrow cannot be passed where an `owned` value is required";

}  // namespace

// ===========================================================================
// THE ISSUE'S OWN REPRO — the spec_0175 shape (spec §26.4's `Sort()` example).
// ===========================================================================

BEZEL_TEST(materializer_local_owned_sink_4042,
           sorted_via_local_return_accepted_spec_28_3_stdlib_4_4) {
    JitResult jr = runAotChecked(
        "module Conf; "
        "class W "
        "  public method Sort(seq: Array<int>): owned Array<int> "
        "    var items := seq.Sorted(); "
        "    return items; "
        "  end Sort; "
        "end W; "
        "class Main "
        "  public static method Main(): int "
        "    using var seq := new Array<int>(); "
        "    seq.Add(3); seq.Add(1); seq.Add(2); "
        "    using var w := new W(); "
        "    using var r := w.Sort(seq); "
        "    return r.Length; "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 3);
}

BEZEL_TEST(materializer_local_owned_sink_4042,
           sorted_via_local_return_move_accepted_spec_28_3_stdlib_4_4) {
    JitResult jr = runAotChecked(
        "module Conf; "
        "class W "
        "  public method Sort(seq: Array<int>): owned Array<int> "
        "    var items := seq.Sorted(); "
        "    return move items; "
        "  end Sort; "
        "end W; "
        "class Main "
        "  public static method Main(): int "
        "    using var seq := new Array<int>(); "
        "    seq.Add(3); seq.Add(1); seq.Add(2); "
        "    using var w := new W(); "
        "    using var r := w.Sort(seq); "
        "    return r.Length; "
        "  end Main; "
        "end Main;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 3);
}

// ===========================================================================
// AMPLIFIED RUNTIME PROOF (300 iterations, ASan/LSan) — one per owned-sink
// SHAPE `ownedArgSourceIsBorrow` gates: return, return move, owned field
// assignment, owned parameter pass. A one-shot run is clean on both sides of
// a bug in this family (measured elsewhere in this codebase); amplifying
// turns even a small per-iteration leak/double-free into an unmistakable
// LSan/ASan report.
// ===========================================================================

BEZEL_TEST(materializer_local_owned_sink_4042,
           amplified_return_toarray_no_leak_stdlib_5) {
    std::string src =
        "module M; "
        "class C "
        "  public static method F(seq: Array<int>): owned Array<int> "
        "    var items := seq.ToArray(); "
        "    return items; "
        "  end F; "
        "  public static method Main(): int "
        "    using var seq := new Array<int>(); "
        "    seq.Add(1); seq.Add(2); seq.Add(3); "
        "    var total := 0; "
        "    for var i in 0..300 do "
        "      using var r := F(seq); "
        "      total := total + r.Length; "
        "    end for; "
        "    return total mod 251; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, (301 * 3) % 251);
}

BEZEL_TEST(materializer_local_owned_sink_4042,
           amplified_return_move_sorted_no_leak_stdlib_4_4) {
    std::string src =
        "module M; "
        "class C "
        "  public static method F(seq: Array<int>): owned Array<int> "
        "    var items := seq.Sorted(); "
        "    return move items; "
        "  end F; "
        "  public static method Main(): int "
        "    using var seq := new Array<int>(); "
        "    seq.Add(3); seq.Add(1); seq.Add(2); "
        "    var total := 0; "
        "    for var i in 0..300 do "
        "      using var r := F(seq); "
        "      total := total + r.Length; "
        "    end for; "
        "    return total mod 251; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, (301 * 3) % 251);
}

BEZEL_TEST(materializer_local_owned_sink_4042,
           amplified_owned_field_assign_reversed_no_leak_stdlib_4_4) {
    std::string src =
        "module M; "
        "class Holder "
        "  public owned Data: Array<int>; "
        "  public constructor(seq: Array<int>) "
        "    var items := seq.Reversed(); "
        "    this.Data := items; "
        "  end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    using var seq := new Array<int>(); "
        "    seq.Add(3); seq.Add(1); seq.Add(2); "
        "    var total := 0; "
        "    for var i in 0..300 do "
        "      using var h := new Holder(seq); "
        "      total := total + h.Data.Length; "
        "    end for; "
        "    return total mod 251; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, (301 * 3) % 251);
}

BEZEL_TEST(materializer_local_owned_sink_4042,
           amplified_owned_param_pass_slice_call_no_leak_stdlib_4_4) {
    std::string src =
        "module M; "
        "class Sink "
        "  public static method Consume(owned x: Array<int>): int "
        "    using var x2 := owned x; "
        "    return x2.Length; "
        "  end Consume; "
        "end Sink; "
        "class C "
        "  public static method F(seq: Array<int>): int "
        "    var items := seq.Slice(0..2); "
        "    return Sink.Consume(items); "
        "  end F; "
        "  public static method Main(): int "
        "    using var seq := new Array<int>(); "
        "    seq.Add(3); seq.Add(1); seq.Add(2); "
        "    var total := 0; "
        "    for var i in 0..300 do "
        "      total := total + F(seq); "
        "    end for; "
        "    return total mod 251; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, (301 * 3) % 251);
}

// ===========================================================================
// GRID — every recognized materializer name (the predicate's own 9-name
// list: ToArray/ToSet/ToDictionary/Sorted/Reversed/Slice/Split/GetBytes/
// ToBytes), through a local, in a method, class AND value element types
// where applicable. Compile+run (ASan, single-shot) — the leak surface for
// the CONTAINER (a trivial-dispose collection struct) is already covered by
// the amplified tests above for the shapes that share it (Array<T>); these
// pin ACCEPTANCE across the remaining materializer names/result heads.
// ===========================================================================

BEZEL_TEST(materializer_local_owned_sink_4042, grid_toset_return_stdlib_5) {
    JitResult jr = runAotChecked(
        "module M; "
        "class C "
        "  public static method F(seq: Array<int>): owned Set<int> "
        "    var items := seq.ToSet(); "
        "    return items; "
        "  end F; "
        "  public static method Main(): int "
        "    using var seq := new Array<int>(); "
        "    seq.Add(1); seq.Add(2); seq.Add(3); "
        "    using var r := F(seq); "
        "    return r.Count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 3);
}

BEZEL_TEST(materializer_local_owned_sink_4042,
           grid_todictionary_return_stdlib_5) {
    JitResult jr = runAotChecked(
        "module M; "
        "class C "
        "  public static method F(seq: Array<int>): owned Dictionary<int, int> "
        "    var items := seq.ToDictionary(lambda(x: int) => x); "
        "    return items; "
        "  end F; "
        "  public static method Main(): int "
        "    using var seq := new Array<int>(); "
        "    seq.Add(1); seq.Add(2); seq.Add(3); "
        "    using var r := F(seq); "
        "    return r.Count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 3);
}

BEZEL_TEST(materializer_local_owned_sink_4042, grid_split_return_stdlib_9_6) {
    JitResult jr = runAotChecked(
        "module M; "
        "class C "
        "  public static method F(s: string): owned Array<string> "
        "    var parts := s.Split(\",\"); "
        "    return parts; "
        "  end F; "
        "  public static method Main(): int "
        "    using var r := F(\"a,b,c\"); "
        "    return r.Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 3);
}

BEZEL_TEST(materializer_local_owned_sink_4042, grid_getbytes_return_stdlib_7) {
    JitResult jr = runAotChecked(
        "module M; "
        "class C "
        "  public static method F(s: string): owned Array<byte> "
        "    var bytes := Encoding.UTF8.GetBytes(s); "
        "    return bytes; "
        "  end F; "
        "  public static method Main(): int "
        "    using var r := F(\"abc\"); "
        "    return r.Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 3);
}

// NOTE — this is a GRID-COMPLETENESS pin, not evidence for this fix.
// `isBuiltinOwnedMaterializerCall`'s allowlist names `ToBytes` (fix #2826),
// but the only stdlib call of that name (`CString.ToBytes()`, stdlib §10.1)
// resolves a REAL `MethodSymbol` (`CString` is a genuinely declared class),
// which the predicate's own narrowness gate (`ctx.exprSymbol(*call.callee)
// != nullptr` -> return false) excludes by construction. So `var bytes :=
// cs.ToBytes(); return bytes;` was NEVER affected by #4042 in the first
// place — `producerState`'s ORDINARY `calleeMethodDecl->returnOwned` arm
// already classifies it Owned, independent of this fix. Measured: this
// exact program compiles clean with ZERO diagnostics on the PRE-FIX
// compiler too (confirmed by reverting src/ownership/Ownership_Checks.cpp
// and rebuilding) — the only FAIL-WITHOUT-verified case in this whole file
// that does NOT flip. Kept here anyway as a grid-completeness pin for the
// predicate's own name list, not as a #4042 regression case.
BEZEL_TEST(materializer_local_owned_sink_4042,
           grid_tobytes_return_unaffected_stdlib_10_1) {
    JitResult jr = runAotChecked(
        "module M; "
        "class C "
        "  public static method F(s: string): owned Array<byte> "
        "    using var cs := CString.From(s); "
        "    var bytes := cs.ToBytes(); "
        "    return bytes; "
        "  end F; "
        "  public static method Main(): int "
        "    using var r := F(\"abc\"); "
        "    return r.Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(jr.ran);
    // "abc" + a null terminator CString buffer; ToBytes() returns the
    // null-terminator-EXCLUSIVE byte copy (stdlib §10.1's `Length` prose),
    // so the expected length is 3.
    BEZEL_EXPECT_EQ(jr.exitCode, 3);
}

BEZEL_TEST(materializer_local_owned_sink_4042,
           grid_class_element_toarray_return_move_stdlib_5) {
    JitResult jr = runAotChecked(
        "module M; "
        "class Elem "
        "  public Tag: int; "
        "  public constructor(t: int) this.Tag := t; end constructor; "
        "end Elem; "
        "class C "
        "  public static method F(seq: Array<Elem>): owned Array<Elem> "
        "    var items := seq.ToArray(); "
        "    return move items; "
        "  end F; "
        "  public static method Main(): int "
        "    using var e1 := new Elem(1); using var e2 := new Elem(2); "
        "    using var seq := new Array<Elem>(); "
        "    seq.Add(e1); seq.Add(e2); "
        "    using var r := F(seq); "
        "    return r.Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 2);
}

BEZEL_TEST(materializer_local_owned_sink_4042,
           grid_class_element_sorted_comparer_return_stdlib_4_4) {
    JitResult jr = runAotChecked(
        "module M; "
        "class Elem "
        "  public Tag: int; "
        "  public constructor(t: int) this.Tag := t; end constructor; "
        "end Elem; "
        "class C "
        "  public static method F(seq: Array<Elem>): owned Array<Elem> "
        "    var items := seq.Sorted(lambda(a: Elem, b: Elem) => a.Tag - b.Tag); "
        "    return items; "
        "  end F; "
        "  public static method Main(): int "
        "    using var e1 := new Elem(2); using var e2 := new Elem(1); "
        "    using var seq := new Array<Elem>(); "
        "    seq.Add(e1); seq.Add(e2); "
        "    using var r := F(seq); "
        "    return r.Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 2);
}

// ===========================================================================
// SETTLE (task item 4): the range-slice INDEX spelling `arr[0..2]` was
// reported by a previous reviewer as NOT reproducing the false-reject on an
// operator. It does not reproduce anywhere, and this pins WHY: `arr[0..2]`
// is an `ast::IndexExpr`, which fix #2753 (v0.240) already gave its OWN
// `producerState` arm — the local's tracked state is ALREADY `Owned`, not
// `Borrowed`, for this ONE spelling (`common::isBuiltinArrayRangeSliceIndex`
// is consulted DIRECTLY by `producerState`, unlike the CallExpr materializer
// predicate this fix's carve-out targets). So `ownedArgSourceIsBorrow`'s
// `lookupState` branch already reads `Owned` and returns `false` (not a
// borrow) with no change needed. This test compiles identically with or
// without this fix — it is a documentation/regression pin, not evidence for
// this change.
// ===========================================================================

BEZEL_TEST(materializer_local_owned_sink_4042,
           settle_range_slice_index_already_owned_via_2753_spec_28_3) {
    JitResult jr = runAotChecked(
        "module M; "
        "class C "
        "  public static method F(seq: Array<int>): owned Array<int> "
        "    var items := seq[0..2]; "
        "    return items; "
        "  end F; "
        "  public static method Main(): int "
        "    using var seq := new Array<int>(); "
        "    seq.Add(3); seq.Add(1); seq.Add(2); "
        "    using var r := F(seq); "
        "    return r.Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 3);
}

// ===========================================================================
// NON-REGRESSION — a GENUINE borrow returned through an `owned` return type
// must still be rejected; this fix's carve-out is keyed on the local's own
// DIRECT initializer being one of the nine recognized builtin materializer
// calls, not on "any Borrowed-state local".
// ===========================================================================

BEZEL_TEST(materializer_local_owned_sink_4042,
           reject_control_plain_borrowed_local_alias_still_rejected_spec_28_3) {
    JitResult jr = runAot(
        "module M; "
        "class C "
        "  public static method F(seq: Array<int>): owned Array<int> "
        "    var alias := seq; "
        "    return alias; "
        "  end F; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kDirectBorrowReturn));
}

BEZEL_TEST(materializer_local_owned_sink_4042,
           reject_control_owned_param_pass_plain_borrow_still_rejected_28_8) {
    JitResult jr = runAot(
        "module M; "
        "class Sink "
        "  public static method Consume(owned x: Array<int>): int "
        "    return x.Length; "
        "  end Consume; "
        "end Sink; "
        "class C "
        "  public static method F(seq: Array<int>): int "
        "    return Sink.Consume(seq); "
        "  end F; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kOwnedArgBorrow));
}

BEZEL_TEST(materializer_local_owned_sink_4042,
           reject_control_owned_field_assign_plain_borrow_still_rejected_28_5) {
    JitResult jr = runAot(
        "module M; "
        "class Holder "
        "  public owned Data: Array<int>; "
        "  public constructor(seq: Array<int>) "
        "    this.Data := seq; "
        "  end constructor; "
        "end Holder;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kOwnedFieldStoreBorrow));
}

// A known, NARROWER residual gap this fix does NOT close: a local bound from
// a further ALIAS of a materializer-bound local (not a DIRECT materializer
// initializer) still reads the raw tracked `Borrowed` state and is still
// rejected. Filed separately (see issue #4053) as a documented non-goal —
// pinning this here so a future fix that widens the carve-out transitively
// must consciously flip this assertion, not accidentally.
BEZEL_TEST(materializer_local_owned_sink_4042,
           transitive_alias_of_materializer_local_still_rejected_known_gap) {
    JitResult jr = runAot(
        "module M; "
        "class C "
        "  public static method F(seq: Array<int>): owned Array<int> "
        "    var items := seq.Sorted(); "
        "    var x := items; "
        "    return x; "
        "  end F; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kDirectBorrowReturn));
}

// `move`-of-a-borrow keeps its own, more specific #1995-lineage diagnostic
// (`checkOwnedReturnBorrowSource`), not the direct-read one — non-regression
// for the message-routing discipline `ownedArgSourceIsBorrow`'s callers rely
// on (this fix touches the SAME shared classifier both routes consult).
BEZEL_TEST(materializer_local_owned_sink_4042,
           move_of_genuine_borrow_keeps_1995_message_not_direct) {
    JitResult jr = runAot(
        "module M; "
        "class C "
        "  public static method F(seq: Array<int>): owned Array<int> "
        "    return move seq; "
        "  end F; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(jr, kMoveOfBorrowReturn));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(jr, kDirectBorrowReturn));
}
