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

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
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
        "    var setX := () => do x := 5; return x; end; "
        "    var _ := setX(); "
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
        "    var f := () => x; "
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
        "    var inc := () => do count := count + 1; return count; end; "
        "    var _ := inc(); "
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
        "    return () => do "
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
        "    var bump := () => do x := x + 2; return x; end; "
        "    var read := () => x; "
        "    var _ := bump(); "   // x := 12 through the shared cell
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
        "    return () => do count := count + 1; return count; end; "
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
BEZEL_TEST(codegen_closures_27_5, loop_variable_fresh_binding) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var fns: [3]() -> int; "
        "    for var i in 1..3 do "
        "      fns[i - 1] := () => i; "  // captures THIS iteration's i
        "    end; "
        "    return fns[0]() + fns[1]() + fns[2](); "  // 1 + 2 + 3 = 6
        "  end Main; "
        "end C;"), 6);
}

// §27.5 / §16.5: if the loop binding were a single shared cell (the classic
// closure-in-loop bug), all three lambdas would observe the final value (3)
// and the sum would be 9. Fresh binding gives 6, so this guards against the
// regression directly.
BEZEL_TEST(codegen_closures_27_5, loop_capture_not_all_final_value) {
    BEZEL_EXPECT_TRUE(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var fns: [3]() -> int; "
        "    for var i in 1..3 do "
        "      fns[i - 1] := () => i; "
        "    end; "
        "    return fns[0]() + fns[1]() + fns[2](); "
        "  end Main; "
        "end C;") != 9);
}

// §27.5: a captured method parameter shares storage too — a write inside the
// lambda is visible to the enclosing method after the call.
BEZEL_TEST(codegen_closures_27_5, captured_parameter_is_shared) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Bump(n: int): int "
        "    var f := () => do n := n + 100; return n; end; "
        "    var _ := f(); "
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
        "    var setter := () => do s := \"hello\"; return 0; end; "
        "    var _ := setter(); "
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
        "    var outer := () => do "
        "      var inner := () => x; "
        "      x := x + 1; "       // mutate between defining and calling inner
        "      return inner(); "   // inner sees the updated x (shared cell)
        "    end; "
        "    return outer(); "     // 11
        "  end Main; "
        "end C;"), 11);
}
