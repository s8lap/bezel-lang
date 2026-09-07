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
// #4057 CORRECTION (post-review): the paragraph above is a NECESSARY but not
// SUFFICIENT safety argument — it covers the DISPOSAL question (does codegen
// ever free this local's value twice), which is genuinely sound as argued.
// It does NOT cover the SEPARATE, CONSUME-TRACKING question: once
// `ownedArgSourceIsBorrow` accepts a materializer-bound local into ONE owned
// sink, does anything stop the SAME local from being accepted into a SECOND
// independent owned sink? A review measured that it did not:
// `handleOwnedFieldAssignment`'s ONLY consume-tracking step (`markMovedByName`
// / `markDisposedByName`) was gated on `state == Owned` — and this carve-out
// deliberately leaves the state Borrowed forever (see above) — so a local
// accepted into one `owned` field (or owning-array slot) was never marked
// consumed, and a SECOND owned-field/owning-slot store of the identical local
// was ALSO silently accepted: two slots, one pointer, two frees at their
// respective owners' disposal — a measured double-free (`free(): invalid
// pointer` / ASan SEGV in `DisposeCore`). The argument-sink path
// (`checkOwnedArgSource`'s CallExpr callers) never had this gap because its
// move-marking is UNCONDITIONAL, not gated on `state == Owned` — passing the
// same materializer local to two owned PARAMETERS was already correctly
// rejected. `handleOwnedFieldAssignment` was the one consumer with the gated
// bookkeeping, and it is fixed in the SAME commit as this correction: it now
// ALSO marks the source consumed (via the shared `isMaterializerCarveOutBorrow`
// fact) when it accepts this carve-out shape, closing both the owned-field
// and the `intoOwningArraySlot=true` cases (the SAME function). See the
// "fix #4057" test section below for the measured repros and the FAIL-WITHOUT
// proof, and `checkOwnedArgSource`/`ownedArgSourceIsBorrow`/
// `handleOwnedFieldAssignment`'s own updated comments for the mechanism.
// // per §21.1, §28.3, §28.5, §28.11, stdlib §5/§4.4/§9.6, #4042, #4057
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
// with the diagnostic text asserted here, and compiled clean post-fix. The
// two "fix #4057" REJECT-repros were separately measured against commit
// a4f881d (#4042 alone, before the #4057 consume-tracking half landed):
// both compiled clean there too, and the field-pair one crashed at runtime.
//
// per §21.1, §28.3, §28.5, §28.6, §28.8, §28.11, stdlib §5 / §4.4 / §9.6,
// v0.294, #4042, #4057

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

// ===========================================================================
// fix #4057 — a MEASURED double-free the #4042 carve-out introduced and this
// same commit closes. `ownedArgSourceIsBorrow`'s carve-out deliberately
// leaves a materializer-bound local's TRACKED STATE at Borrowed forever (see
// that function's own header comment — touching `producerState`/`states_` is
// the double-free direction OwnershipFacts.hpp forbids). `handleOwnedField
// Assignment`'s consume-tracking step was gated on `state == Owned`, so it
// never marked such a local moved-from after a FIRST successful owned-field/
// owning-slot store — a SECOND store of the SAME local was then ALSO
// accepted (the identical Borrowed-forever state, the identical carve-out),
// producing two owned slots holding one pointer.
//
// Measured pre-this-half-of-the-fix (commit a4f881d, #4042 alone): both
// statements below compiled with ZERO diagnostics; running the program
// crashed with `free(): invalid pointer` (plain build) / a SEGV inside the
// containing class's DisposeCore under ASan. Fixed by making
// `handleOwnedFieldAssignment` ALSO mark the source consumed
// (`isMaterializerCarveOutBorrow` + `markMovedByName`) when it accepts this
// carve-out shape — mirroring the argument-sink path's unconditional
// `markMovedByName`, which never had this gap.
// ===========================================================================

// The reviewer's exact repro: two `owned` fields, one materializer-bound
// local. FAIL-WITHOUT proof: measured pre-#4057-half-of-the-fix — compiled
// clean, `free(): invalid pointer` / SIGABRT at runtime (plain build).
BEZEL_TEST(materializer_local_owned_sink_4042,
           two_owned_fields_same_materializer_local_rejected_4057_spec_28_3) {
    JitResult jr = runAot(
        "module M; "
        "class Holder "
        "  public owned Data: Array<int>; "
        "  public owned Other: Array<int>; "
        "  public constructor(seq: Array<int>) "
        "    var items := seq.Sorted(); "
        "    this.Data := items; "
        "    this.Other := items; "
        "  end constructor; "
        "  destructor end destructor; "
        "end Holder;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        jr, "was already moved out at this point"));
}

// The `intoOwningArraySlot=true` sibling the reviewer flagged as structurally
// identical (SAME `handleOwnedFieldAssignment` function, same gated
// bookkeeping) — an owning inline-array slot store `a[i] := items;` twice
// over the SAME materializer-bound local. Measured pre-#4057-half-of-the-fix:
// compiled clean (zero diagnostics); the store itself did not crash (codegen
// took a `bz_array_copy` path for the second store rather than aliasing the
// raw pointer, so THIS particular shape leaked rather than double-freed —
// still a real over-acceptance of a program that must be rejected, from the
// exact same root cause). Fixed by the SAME `handleOwnedFieldAssignment`
// change (both call sites — the owned-field arm and the
// `intoOwningArraySlot=true` arm — share the code this test exercises).
BEZEL_TEST(materializer_local_owned_sink_4042,
           two_owning_array_slots_same_materializer_local_rejected_4057_spec_28_11) {
    JitResult jr = runAot(
        "module M; "
        "class C "
        "  public static method F(seq: Array<int>): int "
        "    var items := seq.Sorted(); "
        "    var a: [2 owned]Array<int>? := [null, null]; "
        "    a[0] := items; "
        "    a[1] := items; "
        "    return 0; "
        "  end F; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(jr));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        jr, "was already moved out at this point"));
}

// PASS-WITH runtime proof: the LEGAL single-store shape (the #4042 grid's
// own `amplified_owned_field_assign_reversed_no_leak_stdlib_4_4` already
// covers the plain owned-field case under amplified ASan) still works after
// #4057's consume-tracking fix — re-affirmed here for TWO fields where only
// ONE receives the materializer-bound local and the other receives an
// independently-produced value, so the fix's new `markMovedByName` call does
// not over-reject a legitimate two-field constructor shape.
BEZEL_TEST(materializer_local_owned_sink_4042,
           two_owned_fields_distinct_sources_still_accepted_no_leak_4057) {
    std::string src =
        "module M; "
        "class Holder "
        "  public owned Data: Array<int>; "
        "  public owned Other: Array<int>; "
        "  public constructor(seq: Array<int>) "
        "    var items := seq.Sorted(); "
        "    this.Data := items; "
        "    this.Other := seq.Reversed(); "
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
        "      total := total + h.Data.Length + h.Other.Length; "
        "    end for; "
        "    return total mod 251; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, (301 * 6) % 251);
}
