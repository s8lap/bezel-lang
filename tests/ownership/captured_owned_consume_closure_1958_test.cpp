// Spec-derived tests for issue #1958 — §28.3 (v0.184) "Captured owned
// bindings".
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
// The bug (#1958): the ownership checker walked lambda bodies only for the
// ESCAPE path (checkClosureEscapesOnReturn), which does not fire for an in-place
// invocation. A marked transfer / `.Dispose()` / discard / reassignment of a
// captured owned binding inside a NON-escaping closure was therefore accepted;
// when the closure is invoked in place the captured owned is disposed by the
// closure and again at the owner's scope end — a double-dispose / heap-use-
// after-free. The repro compiled rc=0 and SIGSEGV'd / ASan-flagged at runtime;
// the identical consume OUTSIDE a closure was already correctly rejected ("cannot
// move out of `using var`"), so the closure body was the gap.
//
// These tests pin the fix: every owned-consuming use of a captured owned binding
// inside a closure (whether or not the closure escapes) is a compile error. The
// positive controls guard against over-rejection: a closure that only BORROWS a
// captured owned binding (reads a field, calls a non-consuming method), and a
// closure that consumes its OWN local / parameter owned binding (not captured),
// must still compile.
//
// Fail-without / pass-with: without the fix the NEG cases compiled with no error
// (accepts → the runtime double-dispose); with the fix each is rejected with the
// §28.3 v0.184 diagnostic. The POS controls compile clean in both directions —
// they guard the fix against false positives.

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

// --- NEG: consuming a captured owned binding inside a closure (REJECT) -----

// The #1958 reproducer: a MARKED TRANSFER `Sink(owned r)` of a captured
// `using var` owned binding inside a closure that is invoked IN PLACE. §28.3
// v0.184: a captured owned binding may not be transferred from. (Without the
// fix this compiled and double-disposed `r` — closure body + `using var` scope
// end.)
BEZEL_TEST(own_captured_owned_1958, marked_transfer_in_closure_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  using var r := new R(); "
        "  var f := lambda() => do return Sink(move r); end; "
        "  return f(); "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCapturedOwnedConsume(r));
}

// The UNMARKED consume `Sink(r)` — a bare identifier (no `owned` marker) flowing
// into an `owned` (consuming) parameter — is the MORE NATURAL spelling and also
// double-disposes. §22.3's call-site `owned`-marker requirement fires only at
// method top level (there `Sink(r)` errors "write Sink(owned r)"); it does NOT
// descend into lambda bodies, so inside a closure neither that check nor the
// marked-form scan caught it before this fix. It must be rejected: the callee
// parameter is `owned`, so the captured owned binding is consumed. (Without the
// fix this compiled and heap-use-after-freed — free in Sink, read in Main.)
BEZEL_TEST(own_captured_owned_1958, unmarked_transfer_in_closure_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  using var r := new R(); "
        "  var f := lambda() => do return Sink(r); end; "
        "  return f(); "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCapturedOwnedConsume(r));
}

// The UNMARKED consume in an expression-statement position (the call result
// discarded) — `_ := Sink(r)` inside the closure — is equally a consume of the
// captured owned binding and equally rejected.
BEZEL_TEST(own_captured_owned_1958, unmarked_transfer_exprstmt_in_closure_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  using var r := new R(); "
        "  var f := lambda() => do _ := Sink(r); return 0; end; "
        "  return f(); "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCapturedOwnedConsume(r));
}

// A MARKED TRANSFER via `move r` inside a closure — the other transfer spelling.
BEZEL_TEST(own_captured_owned_1958, move_transfer_in_closure_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  using var r := new R(); "
        "  var f := lambda() => do return Sink(move r); end; "
        "  return f(); "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCapturedOwnedConsume(r));
}

// An explicit `.Dispose()` of a captured owned binding inside a closure. §28.3
// v0.184 / §28.2: a closure may not dispose a captured owned binding. (Runtime-
// benign via disposed-flag idempotence, but the same root cause — wrongly
// accepted before the fix.)
BEZEL_TEST(own_captured_owned_1958, explicit_dispose_in_closure_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  using var r := new R(); "
        "  var f := lambda() => do r.Dispose(); return 0; end; "
        "  return f(); "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCapturedOwnedConsume(r));
}

// An announced DISCARD `_ := r` of a captured owned binding inside a closure.
// §28.3 v0.184: discard of a captured owned binding is a compile error.
BEZEL_TEST(own_captured_owned_1958, discard_in_closure_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  using var r := new R(); "
        "  var f := lambda() => do _ := r; return 0; end; "
        "  return f(); "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCapturedOwnedConsume(r));
}

// A REASSIGNMENT of a captured owned binding inside a closure. §28.3 v0.184:
// "assignment to it … [is a] compile error." (Rebinding drops the still-owned
// value while the enclosing scope disposes the original — a leak / mismatch.)
BEZEL_TEST(own_captured_owned_1958, reassignment_in_closure_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  var r := new R(); "
        "  var f := lambda() => do r := new R(); return 0; end; "
        "  var n := f(); "
        "  r.Dispose(); "
        "  return n; "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCapturedOwnedConsume(r));
}

// The consume of a captured owned binding inside a NESTED closure is equally an
// error — the binding is still an enclosing-method owned binding there.
BEZEL_TEST(own_captured_owned_1958, transfer_in_nested_closure_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  using var r := new R(); "
        "  var f := lambda() => do "
        "    var g := lambda() => do return Sink(move r); end; "
        "    return g(); "
        "  end; "
        "  return f(); "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCapturedOwnedConsume(r));
}

// The consume can hide inside control flow within the closure body (an `if`);
// the lambda body is opaque to the enclosing CFG, so the walk must descend into
// it. `.Dispose()` inside an `if` inside a closure is still rejected.
BEZEL_TEST(own_captured_owned_1958, dispose_in_closure_control_flow_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  using var r := new R(); "
        "  var f := lambda() => do "
        "    if r.V = 0 then r.Dispose(); end; "
        "    return 0; "
        "  end; "
        "  return f(); "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCapturedOwnedConsume(r));
}

// An `owned` PARAMETER of the enclosing method is also a captured owned binding
// when a closure captures it; consuming it inside the closure is rejected.
BEZEL_TEST(own_captured_owned_1958, capture_owned_param_transfer_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Run(owned p: R): int "
        "  var f := lambda() => do return Sink(move p); end; "
        "  var n := f(); "
        "  return n; "
        "end Run;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsCapturedOwnedConsume(r));
}

// --- POS controls: closures that only BORROW / consume their own (ACCEPT) --

// CONTROL: a closure that only READS a captured owned binding (a value-typed
// field read) is the ordinary borrow-capture case and must compile — reading
// borrows, it does not consume. This is the key false-positive guard.
BEZEL_TEST(own_captured_owned_1958, closure_reads_captured_owned_field_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  using var r := new R(); "
        "  var f := lambda() => do return r.V; end; "
        "  return f(); "
        "end Main;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: a closure that calls a NON-consuming method on the captured owned
// binding borrows it — it does not consume — and must compile.
BEZEL_TEST(own_captured_owned_1958, closure_calls_nonconsuming_method_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  using var r := new R(); "
        "  var f := lambda() => do return r.Get(); end; "
        "  return f(); "
        "end Main;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: a bare identifier passed to a BORROW parameter (`Peek(r)`, `Peek`
// taking a plain `R`, not `owned R`) inside a closure is a legitimate borrow —
// it does not consume — and must compile. This is the key false-positive guard
// for the UNMARKED-consume extension: the unmarked-arg rejection must fire ONLY
// when the callee parameter is `owned`, never for a borrow parameter.
BEZEL_TEST(own_captured_owned_1958, unmarked_arg_to_borrow_param_clean_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Peek(r: R): int return r.V; end Peek; "
        "public static method Main(): int "
        "  using var r := new R(); "
        "  var f := lambda() => do return Peek(r); end; "
        "  return f(); "
        "end Main;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: a closure that consumes its OWN local owned binding (a `using var`
// declared INSIDE the closure body — not captured) must compile. The lambda
// owns that value; disposing / transferring it is legal. Guards the fix against
// mis-flagging a lambda-body-local owned binding.
BEZEL_TEST(own_captured_owned_1958, closure_consumes_own_local_owned_clean_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  var f := lambda() => do "
        "    using var q := new R(); "
        "    return Sink(move q); "
        "  end; "
        "  return f(); "
        "end Main;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: a lambda that consumes its OWN `owned` PARAMETER (not captured) is
// well-formed — the lambda owns that value. Formed and passed to a callback-
// taking method (not invoked here, to isolate the formation from the separate
// function-value-invocation rules). Guards against mis-flagging a lambda's own
// owned parameter.
BEZEL_TEST(own_captured_owned_1958, lambda_consumes_own_owned_param_clean_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Take(cb: (owned R) -> int): int return 0; end Take; "
        "public static method Main(): int "
        "  return Take(lambda(owned x: R) => Sink(move x)); "
        "end Main;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: the identical marked transfer OUTSIDE a closure is (and always was)
// correctly rejected — the closure body was the gap #1958 closed. This pins the
// control the issue names, so a regression that stops rejecting the direct form
// is caught here too.
BEZEL_TEST(own_captured_owned_1958, direct_transfer_of_using_var_still_rejected_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        "public static method Main(): int "
        "  using var r := new R(); "
        "  return Sink(move r); "
        "end Main;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
