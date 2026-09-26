// §27.5 Closures — by-reference (shared, mutable) capture.
//
// "A lambda may reference variables from its enclosing scope. Captured
//  variables are heap-allocated in a closure object. Modifications to a
//  captured variable inside the lambda are visible outside, and vice versa."
//
// "For `for` loop variables, each iteration produces a fresh binding. A lambda
//  captured inside the body sees that iteration's value." (§27.5 / §16.5 line:
//  "The loop variable is freshly bound on each iteration.")
//
// These tests pin the shared-cell semantics: a captured local lives in a heap
// cell shared between the enclosing function and the closure, so reads/writes
// on both sides go through the same storage. They complement lambda_test.cpp
// (which covers non-mutating capture and the closure-struct mechanics).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}

// §27.5: a mutation of a captured local inside the lambda is visible outside
// after the call.
BEZEL_TEST(codegen_closures_27_5, mutation_inside_visible_outside) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var x := 0; "
        "    var setX := lambda() => do x := 5; return x; end; "
        "    _ := setX(); "
        "    return x; "          // sees the lambda's write through the shared cell
        "  end Main; "
        "end C;"), 5);
}

// §27.5: "and vice versa" — a mutation outside, after the lambda is created,
// is seen by the lambda when it later runs.
BEZEL_TEST(codegen_closures_27_5, outer_mutation_seen_inside) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Apply(f: () -> int): int "
        "    return f(); "
        "  end Apply; "
        "  public static method Main(): int "
        "    var x := 1; "
        "    var f := lambda() => x; "
        "    x := 7; "            // written after capture
        "    return Apply(f); "   // lambda observes 7, not the captured-time 1
        "  end Main; "
        "end C;"), 7);
}

// §27.5: the counter example from the spec, adapted to observe the shared
// state from outside. The lambda increments a captured int; calling it twice
// leaves the outer variable at 2.
BEZEL_TEST(codegen_closures_27_5, counter_observed_from_outside) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var count := 0; "
        "    var inc := lambda() => do count := count + 1; return count; end; "
        "    _ := inc(); "
        "    var __ := inc(); "
        "    return count; "      // both increments hit the shared cell
        "  end Main; "
        "end C;"), 2);
}

// §27.5: the spec's MakeCounter — a closure that returns a fresh count on each
// call. The captured `count` lives in the heap closure object and outlives the
// factory method.
BEZEL_TEST(codegen_closures_27_5, make_counter_spec_example) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method MakeCounter(): () -> int "
        "    var count := 0; "
        "    return lambda() => do "
        "      count := count + 1; "
        "      return count; "
        "    end; "
        "  end MakeCounter; "
        "  public static method Main(): int "
        "    var next := MakeCounter(); "
        "    return next() + next() + next(); "  // 1 + 2 + 3 = 6
        "  end Main; "
        "end C;"), 6);
}

// §27.5: two lambdas that capture the same variable share one cell — a write
// through one is visible through the other.
BEZEL_TEST(codegen_closures_27_5, two_lambdas_share_one_variable) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var x := 10; "
        "    var bump := lambda() => do x := x + 2; return x; end; "
        "    var read := lambda() => x; "
        "    _ := bump(); "   // x := 12 through the shared cell
        "    return read(); "     // the other lambda sees 12
        "  end Main; "
        "end C;"), 12);
}

// §27.5: each call to a counter factory yields an *independent* cell — two
// counters do not share state (each `var count` is a distinct binding).
BEZEL_TEST(codegen_closures_27_5, independent_counters) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method MakeCounter(): () -> int "
        "    var count := 0; "
        "    return lambda() => do count := count + 1; return count; end; "
        "  end MakeCounter; "
        "  public static method Main(): int "
        "    var a := MakeCounter(); "
        "    var b := MakeCounter(); "
        "    var x := a() + a(); "  // a: 1 + 2 = 3
        "    var y := b(); "        // b: 1 (independent of a)
        "    return x * 10 + y; "   // 31
        "  end Main; "
        "end C;"), 31);
}

// §27.5 / §16.5: for-loop fresh binding. Each iteration produces a fresh
// binding, so a lambda captured in the body sees that iteration's value. An
// array of lambdas, each built in a distinct iteration, returns distinct
// values rather than all observing the final loop value.
//
// The array carries an explicit initializer: a function type is a non-nullable
// reference type, hence not defaultable (§17.11, v0.307), so `[3]() -> int`
// without an initializer is rejected (§15.1 / §7.9, v0.307; #4419). The
// uninitialised form is pinned as a reject in
// tests/sema/definite_assignment_default_init_4419_test.cpp.
BEZEL_TEST(codegen_closures_27_5, loop_variable_fresh_binding) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var fns: [3]() -> int := [lambda() => 0, lambda() => 0, lambda() => 0]; "
        "    for var i in 1..3 do "
        "      fns[i - 1] := lambda() => i; "  // captures THIS iteration's i
        "    end; "
        "    return fns[0]() + fns[1]() + fns[2](); "  // 1 + 2 + 3 = 6
        "  end Main; "
        "end C;"), 6);
}

// §27.5 / §16.5: if the loop binding were a single shared cell (the classic
// closure-in-loop bug), all three lambdas would observe the final value (3)
// and the sum would be 9. Fresh binding gives 6, so this guards against the
// regression directly.
//
// Asserts the exact value (not merely `!= 9`): `run` returns -999 on a compile
// reject, and a `!= 9` oracle passed vacuously on that (#4419).
BEZEL_TEST(codegen_closures_27_5, loop_capture_not_all_final_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var fns: [3]() -> int := [lambda() => 0, lambda() => 0, lambda() => 0]; "
        "    for var i in 1..3 do "
        "      fns[i - 1] := lambda() => i; "
        "    end; "
        "    return fns[0]() + fns[1]() + fns[2](); "  // 6, never 9
        "  end Main; "
        "end C;"), 6);
}

// §27.5: a captured method parameter shares storage too — a write inside the
// lambda is visible to the enclosing method after the call.
BEZEL_TEST(codegen_closures_27_5, captured_parameter_is_shared) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Bump(n: int): int "
        "    var f := lambda() => do n := n + 100; return n; end; "
        "    _ := f(); "
        "    return n; "          // the param's shared cell now holds n + 100
        "  end Bump; "
        "  public static method Main(): int "
        "    return Bump(5); "    // 105
        "  end Main; "
        "end C;"), 105);
}

// §27.5: a reference-typed (string) captured local is shared by reference too
// — reassigning it inside the lambda is visible outside.
BEZEL_TEST(codegen_closures_27_5, reference_typed_capture_shared) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var s := \"hi\"; "
        "    var setter := lambda() => do s := \"hello\"; return 0; end; "
        "    _ := setter(); "
        "    return s.Length; "   // s now \"hello\" → length 5
        "  end Main; "
        "end C;"), 5);
}

// §27.5: a nested lambda capturing a grandparent-scope local resolves through
// the same shared cell (the capture descends through the intervening lambda).
BEZEL_TEST(codegen_closures_27_5, nested_lambda_capture) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var x := 10; "
        "    var outer := lambda() => do "
        "      var inner := lambda() => x; "
        "      x := x + 1; "       // mutate between defining and calling inner
        "      return inner(); "   // inner sees the updated x (shared cell)
        "    end; "
        "    return outer(); "     // 11
        "  end Main; "
        "end C;"), 11);
}

// §27.5 / #673: for-var loop variable captured by lambda, then REASSIGNED in
// the loop body.  §27.5 fresh binding per iteration: each iteration k has a
// new cell initialised from the loop counter; the body's `k := k + 1`
// modifies that fresh cell; f() returns the UPDATED cell value.
// Expected: f() on iter 0 returns 0+1=1, iter 1 returns 1+1=2, iter 2
// returns 2+1=3; total = 6.  The loop terminates after exactly 3 iterations
// (incBB advances the original slot; condBB checks the slot).
// This shape CRASHED codegen before #673 (miscompile/codegen abort).
BEZEL_TEST(codegen_closures_27_5, for_var_capture_then_reassign_spec_27_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var total: int := 0; "
        "    for var k in 0..2 do "
        "      var f: () -> int := lambda() => k; "
        "      k := k + 1; "          // reassign the for-var loop variable
        "      total := total + f(); " // f sees the updated k
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;"), 6);
}

// §27.5 / #673: named lambda in a while-loop capturing the loop variable
// that is also used as the loop increment.  After boxing with the guarded
// path (non-for-var), all writes to `k` go through the cell.  The
// pre-boxing alloca (read by the while condition in condBB) is kept in
// sync via the cellVarSyncSlot_ write-back so the condition sees updates.
//
// Without the fix: `k := k + 1` writes to the cell only; the original
// alloca stays at 0 → condition `0 < 3` is always true → infinite loop.
// With the fix: each write also syncs to the original alloca → condition
// correctly observes the updated k → loop terminates after 3 iterations.
// After the loop, f() reads the cell (which holds 3) → returns 3.
BEZEL_TEST(codegen_closures_27_5, while_var_capture_and_loop_increment_spec_27_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var k: int := 0; "
        "    var f: () -> int := lambda() => 0; "
        "    while k < 3 do "
        "      f := lambda() => k; "   // capture k via shared cell (guarded boxing)
        "      k := k + 1; "     // writes to cell AND syncs to original alloca
        "    end; "
        "    return f(); "       // cell holds 3 after 3 increments
        "  end Main; "
        "end C;"), 3);
}

// §27.5 / #673: while-loop with IIFE capturing the loop counter variable
// (the pattern that triggered the original pre-existing crash report in
// commit ee35cdd via the #429 activation lane).  The loop variable `k`
// is captured by the IIFE and also serves as the loop increment.  The
// IIFE returns k*k; the loop accumulates the squares of 0..4.
// Without the fix: infinite loop (condition reads stale alloca).
// With the fix: terminates; total = 0+1+4+9+16 = 30.
BEZEL_TEST(codegen_closures_27_5, while_iife_capture_and_loop_increment_spec_27_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var k: int := 0; "
        "    var total: int := 0; "
        "    while k < 5 do "
        "      total := total + (lambda() => k * k)(); " // IIFE captures k
        "      k := k + 1; "                        // sync keeps condBB in sync
        "    end; "
        "    return total; "    // 0+1+4+9+16 = 30
        "  end Main; "
        "end C;"), 30);
}

// §27.5/§22.3 / #1207: a `ref` parameter captured by a lambda must alias
// the caller's storage — the lambda's write goes through to the caller's
// variable.  Before the fix, refParams_ was never consulted by the capture
// loop (only paramSlots_ and locals_), so the symbol was silently dropped
// from the closure env and the lambda's write was lost (rc=10, not 110).
BEZEL_TEST(codegen_closures_27_5, ref_param_captured_by_lambda_1207) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Bump(ref x: int): int "
        "    var f := lambda() => do x := x + 100; return 0; end; "
        "    var i := f(); "
        "    return x; "   // must see the lambda's write: 10 + 100 = 110
        "  end Bump; "
        "  public static method Main(): int "
        "    var v := 10; "
        "    return Bump(ref v); "
        "  end Main; "
        "end C;"), 110);
}

// §27.5/§22.3 / #1207: same as above for `out` parameters — the lambda's
// write reaches the caller's storage (3 + 7 = 10; before the fix rc=3).
BEZEL_TEST(codegen_closures_27_5, out_param_captured_by_lambda_1207) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Produce(out x: int): int "
        "    x := 3; "
        "    var f := lambda() => do x := x + 7; return 0; end; "
        "    var i := f(); "
        "    return x; "   // must see the lambda's write: 3 + 7 = 10
        "  end Produce; "
        "  public static method Main(): int "
        "    var v: int; "
        "    return Produce(out v); "
        "  end Main; "
        "end C;"), 10);
}

// §27.5/§22.3 / #1207: read back via the outer method after the lambda runs
// — confirms the write-back propagates to the caller's variable (not just
// the local copy within the method).
BEZEL_TEST(codegen_closures_27_5, ref_param_capture_visible_to_caller_1207) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method BumpRef(ref x: int): int "
        "    var f := lambda() => do x := x + 100; return 0; end; "
        "    var i := f(); "
        "    return 0; "
        "  end BumpRef; "
        "  public static method Main(): int "
        "    var v := 10; "
        "    _ := BumpRef(ref v); "
        "    return v; "   // caller observes 110 — write went through ref
        "  end Main; "
        "end C;"), 110);
}

// §27.5 / #1135: grandparent-scope local escaped by a nested lambda
// (two-level nesting). The outer lambda captures x and returns an inner
// lambda that also captures x. The inner lambda escapes Make() — but x must
// be heap-promoted into a cell that stays live until the inner lambda dies.
// Before the fix: cellFreeEligible_ incorrectly treated x as non-escaping
// (the outer lambda is released at Make()'s scope exit, but the same cell is
// held by the inner lambda's env) → bz_box_free on the cell while the inner
// lambda still referenced it → UAF / WRONG_VALUE.
BEZEL_TEST(codegen_closures_27_5_1135,
           nested_lambda_grandparent_local_escapes_2level) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Make(): () -> int "
        "    var x := 3; "
        "    var outer := lambda() => lambda() => x; "   // inner captures grandparent x
        "    return outer(); "               // inner lambda escapes Make
        "  end Make; "
        "  public static method Main(): int "
        "    var inner := Make(); "
        "    return inner(); "               // expected 3 (stable, not UAF garbage)
        "  end Main; "
        "end C;"), 3);
}

// §27.5 / #1135: three-level nest — innermost lambda escapes through two
// enclosing lambdas. The grandparent local x must stay live until the
// innermost lambda is released.
BEZEL_TEST(codegen_closures_27_5_1135,
           nested_lambda_grandparent_local_escapes_3level) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Make(): () -> int "
        "    var x := 7; "
        "    var a := lambda() => lambda() => lambda() => x; "  // 3-level nest; x is grandgrand-parent
        "    var b := a(); "                  // b = () => () => x
        "    return b(); "                    // returns inner () => x, escapes
        "  end Make; "
        "  public static method Main(): int "
        "    var f := Make(); "
        "    return f(); "                    // expected 7
        "  end Main; "
        "end C;"), 7);
}

// §27.5 / #1135: two grandparent locals both captured by escaped nested
// lambda. Both cells must remain live.
BEZEL_TEST(codegen_closures_27_5_1135,
           nested_lambda_two_grandparent_locals_escape) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Make(): () -> int "
        "    var a := 10; "
        "    var b := 5; "
        "    var outer := lambda() => lambda() => a - b; "
        "    return outer(); "
        "  end Make; "
        "  public static method Main(): int "
        "    var f := Make(); "
        "    return f(); "                    // expected 5
        "  end Main; "
        "end C;"), 5);
}

// §27.5 / #1135: regression — single-level escaping capture still works.
// A lambda directly capturing a local and being returned must read the
// correct value (this worked before the fix and must continue to work).
BEZEL_TEST(codegen_closures_27_5_1135,
           single_level_escaping_capture_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Make(): () -> int "
        "    var x := 42; "
        "    var f := lambda() => x; "
        "    return f; "           // single-level escape — pre-existing, must work
        "  end Make; "
        "  public static method Main(): int "
        "    var f := Make(); "
        "    return f(); "         // expected 42
        "  end Main; "
        "end C;"), 42);
}

// §27.5 / #1135: non-escaping nested lambda (invoked locally, not escaped)
// must return the correct value. This tests that the fix does not break the
// working non-escaping nested-lambda case.
BEZEL_TEST(codegen_closures_27_5_1135,
           nested_lambda_non_escaping_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var x := 9; "
        "    var outer := lambda() => do "
        "      var inner := lambda() => x; "
        "      return inner(); "    // inner is invoked locally, not escaped
        "    end; "
        "    return outer(); "      // expected 9
        "  end Main; "
        "end C;"), 9);
}

// §27.5 / #1135: grandparent outer PARAM (not a local) captured by escaped
// inner lambda must read the correct value.
BEZEL_TEST(codegen_closures_27_5_1135,
           nested_lambda_grandparent_param_escapes) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Make(n: int): () -> int "
        "    var outer := lambda() => lambda() => n; "
        "    return outer(); "
        "  end Make; "
        "  public static method Main(): int "
        "    var f := Make(13); "
        "    return f(); "          // expected 13
        "  end Main; "
        "end C;"), 13);
}
