// Spec-derived regression tests for issue #4156 — the checked-disposal
// diagnostic (§28.2) has an architectural gap for a capture-escaping
// binding: it is a SINGLE-FUNCTION dominance analysis, so it cannot see a
// read that happens only inside a closure body invoked at an unknowable
// future point.
//
// Filed while fixing #4136 (a codegen stack-buffer-overflow in
// `entry.Dispose()`'s captured-`DrainedEntry` arm). With #4136's overflow
// gone, the same program takes a null-pointer SEGV *inside the invoked
// closure*, reading a `Key` field `Dispose()` had already nulled — and the
// compiler said nothing:
//
//     var f := lambda() => entry.Key.Length;
//     entry.Dispose();
//     return f;
//
// #4156's own escalation comment localised the gap by contrast: the
// NON-captured spelling of this shape (`entry.Dispose(); return
// entry.Key.Length;`) already gets `checkRead`'s §28.2 dominating-Dispose
// WARNING; the captured spelling got nothing at all.
//
// Root cause (traced, not the issue's own guess): `checkIndirectEscapeOnReturn`
// (the `return f;` bare-holder half of the §28.9 return-escape family,
// `Ownership_Checks.cpp`) already computes exactly the fact needed — Shape 3
// of `borrowSourceForExpr` records `f`'s captured source (`entry`) when the
// lambda literal is created, and `checkIndirectEscapeOnReturn` re-checks that
// source's LIVE state at the return. But its guard only asked "is the source
// still a live escape-bound obligation" (`isEscapeBoundLocal`, `state ==
// Owned`) — a captured local the enclosing scope had ALREADY explicitly
// disposed was no longer Owned, so the guard's own "no live borrow to
// dangle" comment silently assumed the disposal was safe. It is not: a
// closure captures its source by reference into a SHARED, MUTATED cell
// (`Dispose()` nulls the field, #4136), so every future invocation reads the
// disposal regardless of when it happened relative to the closure literal.
//
// The fix adds a narrowly-scoped arm (`isDisposedTrackedLocal`, gated on a
// function-typed holder specifically) that fires when the source is already
// Disposed, at the SAME Error severity as this function's sibling arms
// (catch-borrow escape, ref/out escape, borrow-bearing-record escape, and
// the pre-existing "still Owned" arm just below it) — a completion of an
// ALREADY-Error §28.9 rule, not a new severity decision on the separately
// open §3975/§3979/§4023 warning-vs-error question (which is about
// `checkRead`'s OWN split across read positions in a single function, a
// different code path this fix does not touch).
//
// Measured NOT extended (and why): a direct-lambda-literal twin
// (`entry.Dispose(); return lambda() => entry.Key.Length;`) was probed and
// found to already be rejected by a DIFFERENT, pre-existing mechanism
// (`reportMovedFromCapturesIn`'s #3771 Disposed leg, §28.2/§28.3, Warning
// severity) — that check fires per-STATEMENT at the closure literal's own
// creation point, which for a directly-embedded literal coincides exactly
// with the return, so it always pre-empts a same-shaped extension of
// `checkClosureEscapesOnReturn`'s Owned-arm. That extension was written,
// found unreachable by every probe, and reverted rather than shipped untested
// (see the investigation note left in `checkClosureEscapesOnReturn`,
// Ownership_Checks.cpp, next to its `st.state != OwnershipState::Owned`
// gate). A DEEPER, separate gap surfaced during that investigation — the
// `return mk();` (#3702) indirection shape silently accepts a captured
// disposed local when the capturing literal lives inside a DIFFERENT
// lambda body than the outer return — and is filed separately as issue
// 4175, out of scope for this fix.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// A plain class with a `V(): int` instance method — the same minimal
// fixture #3702's sibling suite uses, so `r` is a §28.3 scope-bound owned
// local whose capture is a §28.9 borrow with no other rule in play.
std::string wrap(const std::string& method) {
    return std::string(
        "module M; "
        "class R "
        "  public method V(): int return 5; end V; "
        "end R; "
        "class C ") + method + " end C;";
}

}  // namespace

// POSITIVE (§28.2 / §28.9, #4156): the core fix. `f` is bound from a lambda
// literal while `r` is still Owned (no diagnostic at the LocalDecl), `r` is
// then explicitly disposed, and `f` — still just a holder of the captured
// closure — is returned. Before the fix: rc=0, zero diagnostics (measured on
// the parent). Any future call through the returned closure reads `r` after
// its Dispose() — exactly #4156's SEGV shape, one field access simplified.
BEZEL_TEST(closure_capture_disposed_indirect_escape,
           holder_disposed_before_return_rejected_4156) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Mk(): () -> int "
        "  using var r := new R(); "
        "  var f := lambda(): int => r.V(); "
        "  r.Dispose(); "
        "  return f; "
        "end Mk;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "is a closure capturing `r`, which was explicitly disposed "
           "before this return"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "(§28.2 dominating-Dispose rule, extended to a capture-escaping "
           "binding per §28.3"));
}

// CONTROL (§28.9, pre-existing, unaffected by this fix): the NEVER-disposed
// sibling. `r` is still Owned (not Disposed) at the return, so this is the
// PRE-EXISTING "captures an owned local whose source is disposed at scope
// end" arm this fix's new arm sits beside, not the new one — pinned so a
// future edit cannot silently fold the two arms together and lose the
// distinct wording either diagnosis needs.
BEZEL_TEST(closure_capture_disposed_indirect_escape,
           holder_never_disposed_still_rejected_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Mk(): () -> int "
        "  using var r := new R(); "
        "  var f := lambda(): int => r.V(); "
        "  return f; "
        "end Mk;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "carries a borrow of `r` whose source is disposed at scope end "
           "— the value aliases an owned scope-bound local and returning "
           "it lets the borrow outlive that local (§28.3 / §9.7 "
           "borrow-lifetime rule"));
}

// NEGATIVE (§28.9, #4156 accept-side sweep — LANE_RULES §15): the closure is
// invoked LOCALLY (its result bound to `v`) BEFORE `r` is disposed, and only
// the plain int `v` — not the closure — is returned. `checkIndirectEscapeOnReturn`
// never even resolves `f` as the returned expression's root, so this must
// stay accepted regardless of the new Disposed arm.
BEZEL_TEST(closure_capture_disposed_indirect_escape,
           holder_invoked_before_dispose_int_result_returned_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Mk(): int "
        "  using var r := new R(); "
        "  var f := lambda(): int => r.V(); "
        "  var v := f(); "
        "  r.Dispose(); "
        "  return v; "
        "end Mk;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// NEGATIVE (§28.9, #4156 accept-side sweep): the closure is disposed-then-
// CALLED in the return statement itself (`return f();`), not returned as a
// holder. The returned expression is a CallExpr, not the bare identifier
// `checkIndirectEscapeOnReturn` requires — mirrors #3702's own
// `mk_call_discarded_not_returned_accepted_28_9` boundary for the sibling
// direct-lambda-literal check. Must stay accepted: the closure's one
// invocation happens synchronously, in this frame, before `Mk` returns.
BEZEL_TEST(closure_capture_disposed_indirect_escape,
           holder_disposed_then_call_result_returned_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Mk(): int "
        "  using var r := new R(); "
        "  var f := lambda(): int => r.V(); "
        "  r.Dispose(); "
        "  return f(); "
        "end Mk;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// NEGATIVE (§28.9, #4156 accept-side sweep): the returned closure captures a
// DIFFERENT, unrelated int parameter — never `r` at all. `r` is disposed
// before the return, but `f`'s tracked source (Shape 3 of
// `borrowSourceForExpr`) is never set to `r` because the lambda literal
// never reads it, so `isDisposedTrackedLocal` is never even asked about `r`
// for this holder. Must stay accepted.
BEZEL_TEST(closure_capture_disposed_indirect_escape,
           holder_unrelated_capture_accepted) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Mk(other: int): () -> int "
        "  using var r := new R(); "
        "  var f := lambda(): int => other + 1; "
        "  r.Dispose(); "
        "  return f; "
        "end Mk;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL (§28.3 / §28.9, pre-existing, unaffected by this fix): `r` is
// disposed on ONE branch only. §28.3's OWNED ⊓ DISPOSED join resolves to
// plain Owned (with a separate `disposedOnSomePath` flag `checkRead`
// consults, not `checkIndirectEscapeOnReturn`) — so at the return, `r`'s
// state is Owned, not Disposed, and this falls into the PRE-EXISTING
// "still Owned" arm exactly as the never-disposed control above. Pinned so a
// future change to the join semantics cannot silently make this hit (or
// silently stop hitting) the new Disposed-specific arm.
BEZEL_TEST(closure_capture_disposed_indirect_escape,
           holder_partial_branch_dispose_still_rejected_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Mk(cond: bool): () -> int "
        "  using var r := new R(); "
        "  var f := lambda(): int => r.V(); "
        "  if cond then "
        "    r.Dispose(); "
        "  end; "
        "  return f; "
        "end Mk;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "whose source is disposed at scope end"));
}

// COVERAGE (issue #4197, filling a gap left by #4156/#4136): the loop-
// CARRIED `DrainedEntry<K,V>` binding of `for var entry in d.Drain() do
// ... end` (stdlib §4.9) is a DIFFERENT declaration/capture-boxing shape
// than the plain `using var` local every row above exercises — a distinct
// AST node (a for-loop variable, not a LocalDecl), a per-ITERATION scope
// rather than a method-body scope, and (per fix #4136's own header comment
// in tests/codegen/dict_drain_entry_dispose_capture_4136_test.cpp) a
// closure capturing it forces §27.5 cell-boxing of ITS OWN dedicated kind,
// routed through a codegen-level `boxedStorage` GEP that #4136 fixed only
// months after the plain-local capture path was already correct. Issue
// #4136's SEGV repro and issue #4156's fix were BOTH motivated by exactly
// this shape, yet no test anywhere in the tree exercised "return a closure
// over a disposed DrainedEntry for-loop variable" — #4156's own regression
// suite (above) used only a minimal generic `using var` fixture, and
// #4136's suite (tests/codegen/dict_drain_entry_dispose_capture_4136_test.cpp)
// deliberately never returns its capturing closures (see that file's header,
// "the two programs below deliberately avoid invoking a closure that reads
// a field Dispose() has released"). `checkIndirectEscapeOnReturn`'s
// Disposed-source arm (Ownership_Checks.cpp, `isDisposedTrackedLocal`) is a
// SYMBOL-STATE check keyed on `states_[sym]`, not an AST-node-kind walk, so
// there is no known reason it would treat a for-loop variable's symbol
// differently from a `using var` local's — but that is exactly the kind of
// assumption this project's regression-test discipline says must be PINNED,
// not left to reasoning by analogy (issue #4197's own filing, echoing
// #4136's header: "the DrainedEntry for-loop-variable path has its own,
// previously-undetected, codegen-level peculiarities that a generic `using
// var` fixture did not exercise"). Both a K=int and a K=string
// `DrainedEntry<K,V>` loop variable are pinned below, per #4197's proposal.
//
// `Widget` (a class T for `V: class`, stdlib §4.9's OwningDictionary/
// DrainedEntry constraint) mirrors dict_drain_entry_dispose_capture_4136_
// test.cpp's own fixture exactly, so this row exercises the identical
// capture-boxing shape that file's rows exercise, just returned instead of
// invoked-then-discarded.

BEZEL_TEST(closure_capture_disposed_indirect_escape,
           drain_entry_int_key_loop_var_disposed_before_return_rejected_4156_4197) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public class Widget "
        "  public property N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "end Widget; "
        "class C "
        "  public static method Mk(): () -> int "
        "    using var d := new OwningDictionary<int, Widget>(); "
        "    d.Add(7, new Widget(9)); "
        "    for var entry in d.Drain() do "
        "      var f := lambda(): int => entry.Key; "
        "      entry.Dispose(); "
        "      return f; "
        "    end; "
        "    return lambda(): int => 0; "
        "  end Mk; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "is a closure capturing `entry`, which was explicitly disposed "
           "before this return"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "(§28.2 dominating-Dispose rule, extended to a capture-escaping "
           "binding per §28.3"));
}

BEZEL_TEST(closure_capture_disposed_indirect_escape,
           drain_entry_string_key_loop_var_disposed_before_return_rejected_4156_4197) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public class Widget "
        "  public property N: int; "
        "  public constructor(n: int) N := n; end constructor; "
        "end Widget; "
        "class C "
        "  public static method Mk(): () -> int "
        "    using var d := new OwningDictionary<string, Widget>(); "
        "    d.Add(\"xy\", new Widget(9)); "
        "    for var entry in d.Drain() do "
        "      var f := lambda(): int => entry.Key.Length; "
        "      entry.Dispose(); "
        "      return f; "
        "    end; "
        "    return lambda(): int => 0; "
        "  end Mk; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "is a closure capturing `entry`, which was explicitly disposed "
           "before this return"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "(§28.2 dominating-Dispose rule, extended to a capture-escaping "
           "binding per §28.3"));
}
