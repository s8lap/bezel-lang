// Spec-derived tests for issue #1967 — §27 (closures, v0.184) + §28.3
// (Captured owned bindings, v0.184) "IIFE in if/while condition consumes
// captured owned".
//
// Normative text (spec §28.3, v0.184, "Captured owned bindings"):
//   "Inside a closure, a captured owned binding — an owned local or `owned`
//    parameter of the enclosing body — is uniformly in **borrow state**: reads
//    borrow …, and assignment to it, a marked transfer from it, an explicit
//    `Dispose()` or discard of it, and every other owned-consuming use of it are
//    compile errors — a closure cannot consume, reassign, or dispose a captured
//    owned binding."
// Appendix C (v0.184): "A closure may read a captured owned binding (as a
//    borrow) but may not assign to it, transfer from it, or dispose it."
//
// The bug (#1967): the captured-owned-consume scan runs only from `visitStmt`'s
// statement prologue, where it enters every LambdaExpr at top level (the #1958
// fix). But a lambda literal placed DIRECTLY in an `if`/`while` CONDITION — an
// immediately-invoked function expression, "IIFE" — is reached through a
// `CallExpr` whose callee is the lambda, not through a top-level LambdaExpr.
// The CallExpr handler recursed into the callee without flipping `insideLambda`
// to true, so the lambda body was scanned with `insideLambda == false` and the
// consume went unrejected. The repro compiled rc=0 and ASan-flagged
// heap-use-after-free at scope end (closure disposed `r`, `using var` then
// disposed it again). #1958 closed the statement-body case; this is the
// controlling-expression case (an if/while/for condition, a catch guard — the
// handleDischarge path, which the visitStmt prologue never walks).
//
// These tests pin the fix: a lambda IIFE in an `if`/`while` condition that
// consumes a captured owned binding is a compile error (§28.3 v0.184 + §27).
// The positive controls guard against over-rejection: an IIFE that only
// BORROWS the captured owned binding (reads a field), and an IIFE that
// consumes its OWN local owned binding (not captured), must still compile.
// The unrelated IIFE patterns (a `Result<T,E>.Bind` callback, a passed-in
// lambda called immediately) are unaffected.
//
// Fail-without / pass-with: without the fix the NEG cases compiled with no
// error (accepts → the runtime double-dispose); with the fix each is rejected
// with the §28.3 v0.184 diagnostic. The POS controls compile clean in both
// directions — they guard the fix against false positives.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::Severity;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// A class R with a destructor — a genuine non-trivial-dispose resource whose
// double-dispose is the soundness hazard. `Sink` is an owned-consuming sink.
std::string wrap(const std::string& body) {
    return std::string(
        "module M; "
        "class R "
        "  public V: int := 0; "
        "  public method Get(): int return V; end Get; "
        "  destructor end destructor; "
        "end R; "
        "class C "
        "  public static method Sink(owned r: R): int "
        "    r.Dispose(); return 0; "
        "  end Sink; "
        ) + body + " end C;";
}

// True iff the ownership pass emitted the §28.3 v0.184 captured-owned-consume
// error. Makes each NEG test load-bearing on THIS diagnostic (a fail-without
// build, which accepted the consume, produces no such message).
bool rejectsCapturedOwnedConsume(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != Severity::Error) continue;
        if (d.message.find("captured owned binding") != std::string::npos &&
            d.message.find("§28.3, v0.184") != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

// --- NEG: IIFE in if/while condition consuming a captured owned (REJECT) ----

// The #1967 reproducer: an IIFE in an `if` CONDITION — `(() => do return
// Sink(owned r); end)()` — that consumes a captured `using var` owned binding.
// §28.3 v0.184: a captured owned binding may not be transferred from. (Without
// the fix this compiled and double-disposed `r` — closure body + `using var`
// scope end → ASan heap-use-after-free.)
BEZEL_TEST(own_captured_owned_1967, iife_in_if_condition_transfer_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  using var r := new R(); "
        "  if (lambda() => do return Sink(move r); end)() = 0 then return 0; else return 1; end; "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCapturedOwnedConsume(r));
}

// Same shape but in a `while` CONDITION — the second of the two patterns the
// issue names. The closure is invoked every iteration; the consume double-
// disposes `r` on the first iteration's IIFE invocation AND at scope end.
BEZEL_TEST(own_captured_owned_1967, iife_in_while_condition_transfer_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  using var r := new R(); "
        "  while (lambda() => do return Sink(move r); end)() = 0 do return 0; end; "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCapturedOwnedConsume(r));
}

// The UNMARKED consume `Sink(r)` inside the IIFE in an `if` condition — the
// more natural spelling. The callee parameter is `owned`, so the captured
// owned binding is consumed even though the call site omits the `owned` marker
// (the §22.3 call-site-marker check fires only at method top level — the same
// gap as #1958 / unmarked-transfer-in-closure). Must be rejected.
BEZEL_TEST(own_captured_owned_1967, iife_in_if_condition_unmarked_transfer_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  using var r := new R(); "
        "  if (lambda() => do return Sink(r); end)() = 0 then return 0; else return 1; end; "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCapturedOwnedConsume(r));
}

// Explicit `.Dispose()` of a captured owned binding inside an IIFE condition.
// §28.3 v0.184 / §28.2: a closure may not dispose a captured owned binding.
BEZEL_TEST(own_captured_owned_1967, iife_in_if_condition_dispose_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  using var r := new R(); "
        "  if (lambda() => do r.Dispose(); return true; end)() then return 0; else return 1; end; "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCapturedOwnedConsume(r));
}

// Announced DISCARD `_ := r` of a captured owned binding inside an IIFE
// condition. §28.3 v0.184: discard of a captured owned binding is a compile
// error.
BEZEL_TEST(own_captured_owned_1967, iife_in_if_condition_discard_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  using var r := new R(); "
        "  if (lambda() => do _ := r; return true; end)() then return 0; else return 1; end; "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCapturedOwnedConsume(r));
}

// An `owned` PARAMETER of the enclosing method, captured by an IIFE condition
// lambda and consumed inside — same gap on the parameter side.
BEZEL_TEST(own_captured_owned_1967, iife_in_if_condition_capture_owned_param_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Run(owned p: R): int "
        "  if (lambda() => do return Sink(move p); end)() = 0 then return 0; else return 1; end; "
        "  return 0; "
        "end Run;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCapturedOwnedConsume(r));
}

// --- POS controls: IIFEs that only BORROW / consume their own (ACCEPT) -----

// CONTROL: an IIFE in an `if` condition that only READS a captured owned
// binding (a value-typed field read) is the ordinary borrow-capture case and
// must compile — reading borrows, it does not consume. This is the key
// false-positive guard for the fix.
BEZEL_TEST(own_captured_owned_1967, iife_in_if_condition_reads_captured_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  using var r := new R(); "
        "  if (lambda() => do return r.V; end)() = 0 then return 0; else return 1; end; "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: an IIFE that calls a NON-consuming method on the captured owned
// binding borrows it — it does not consume — and must compile.
BEZEL_TEST(own_captured_owned_1967, iife_in_if_condition_calls_nonconsuming_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  using var r := new R(); "
        "  if (lambda() => do return r.Get(); end)() = 0 then return 0; else return 1; end; "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: a bare identifier passed to a BORROW parameter (`Peek(r)`, `Peek`
// taking a plain `R`, not `owned R`) inside an IIFE in an `if` condition is a
// legitimate borrow — it does not consume — and must compile. Pins the
// unmarked-arg rejection must fire ONLY when the callee parameter is `owned`.
BEZEL_TEST(own_captured_owned_1967, iife_in_if_condition_unmarked_to_borrow_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Peek(r: R): int return r.V; end Peek; "
        "public static method Main(): int "
        "  using var r := new R(); "
        "  if (lambda() => do return Peek(r); end)() = 0 then return 0; else return 1; end; "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: an IIFE that consumes its OWN local owned binding (a `using var`
// declared INSIDE the closure body — not captured) must compile. The lambda
// owns that value; transferring it is legal. Guards the fix against
// mis-flagging a lambda-body-local owned binding.
BEZEL_TEST(own_captured_owned_1967, iife_in_if_condition_consumes_own_local_clean_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  if (lambda() => do "
        "    using var q := new R(); "
        "    return Sink(move q); "
        "  end)() = 0 then return 0; else return 1; end; "
        "  return 0; "
        "end Main;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}