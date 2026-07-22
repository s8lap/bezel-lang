// §27.5 Closures — a recursive lambda that calls itself through the local it is
// assigned to (issue #579).
//
// "A lambda may reference variables from its enclosing scope. Captured
//  variables are heap-allocated in a closure object. Modifications to a
//  captured variable inside the lambda are visible outside, and vice versa."
//  (§27.5)
//
// The "and vice versa" clause is what makes a recursive self-reference work:
//
//   var counter: (int) -> int;
//   counter := (n: int) => do
//     if n <= 0 then return 0; end;
//     return counter(n - 1) + 1;     // refers to the captured `counter`
//   end;
//
// The lambda captures `counter` by reference (a shared heap cell, §27.5). After
// the `:=` stores the finalized closure into that shared cell, the lambda body
// — which reads `counter` through the same cell — sees the closure it is part
// of, so the recursive call resolves correctly.
//
// REGRESSION GUARD (#579): before the fix, codegen computed the assignment's
// LHS address BEFORE lowering the RHS lambda. Lowering the lambda boxed the
// `counter` slot (promoting it to a shared cell and rebinding the local), but
// the already-resolved LHS address still pointed at the original un-boxed slot.
// The store wrote the closure into the stale slot while the captured cell held
// pre-box garbage; the recursive call dereferenced that garbage → SIGSEGV.

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

// §27.5 / #579: the spec issue's exact example. counter(5) counts down to 0 and
// adds 1 on each unwind: counter(5) = counter(0) + 5 = 5.
BEZEL_TEST(codegen_recursive_lambda_27_5, counter_self_reference_579) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var counter: (int) -> int; "
        "    counter := lambda(n: int) => do "
        "      if n <= 0 then return 0; end; "
        "      return counter(n - 1) + 1; "
        "    end; "
        "    return counter(5); "
        "  end Main; "
        "end C;"), 5);
}

// §27.5 / #579: deeper recursion exercises the cell-resolved self-call on every
// level. counter(10) = 10.
BEZEL_TEST(codegen_recursive_lambda_27_5, counter_deeper_recursion) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var counter: (int) -> int; "
        "    counter := lambda(n: int) => do "
        "      if n <= 0 then return 0; end; "
        "      return counter(n - 1) + 1; "
        "    end; "
        "    return counter(10); "
        "  end Main; "
        "end C;"), 10);
}

// §27.5 / #579: recursive factorial via a self-referencing lambda.
// fact(5) = 120. Multiplies on the unwind through the captured `fact`.
BEZEL_TEST(codegen_recursive_lambda_27_5, factorial_self_reference) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var fact: (int) -> int; "
        "    fact := lambda(n: int) => do "
        "      if n <= 1 then return 1; end; "
        "      return n * fact(n - 1); "
        "    end; "
        "    return fact(5); "
        "  end Main; "
        "end C;"), 120);
}

// §27.5 / #579: the base case alone — a self-referencing lambda whose first call
// never recurses still resolves the captured slot to the assigned closure.
BEZEL_TEST(codegen_recursive_lambda_27_5, base_case_only) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var counter: (int) -> int; "
        "    counter := lambda(n: int) => do "
        "      if n <= 0 then return 0; end; "
        "      return counter(n - 1) + 1; "
        "    end; "
        "    return counter(0); "        // base case: 0
        "  end Main; "
        "end C;"), 0);
}

// §27.5 / #579: a block-bodied recursive sum (1..n) via the captured `sum`.
// Note: an expression-bodied form `sum := (n: int) => 0 if n <= 0 else
// n + sum(n - 1)` also parses and is tested separately in
// lambda_conditional_body_589_test.cpp (fix #589). The block body is kept
// here because this test's focus is the §27.5 recursive-capture semantics,
// not the conditional-expression parsing.
BEZEL_TEST(codegen_recursive_lambda_27_5, recursive_sum) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var sum: (int) -> int; "
        "    sum := lambda(n: int) => do "
        "      if n <= 0 then return 0; end; "
        "      return n + sum(n - 1); "
        "    end; "
        "    return sum(5); "          // 1+2+3+4+5 = 15
        "  end Main; "
        "end C;"), 15);
}

// --- Regression: the ordinary (non-recursive) capture paths must be unchanged.

// §27.5: a plain non-recursive lambda assigned to a local and called once.
// Guards against the reordered eval path breaking the common case.
BEZEL_TEST(codegen_recursive_lambda_27_5, non_recursive_lambda_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var addOne: (int) -> int; "
        "    addOne := lambda(n: int) => n + 1; "
        "    return addOne(41); "
        "  end Main; "
        "end C;"), 42);
}

// §27.5: a closure capturing a NORMAL local (not itself). The lambda reads a
// captured `offset`; the recursive-lambda eval reorder must not disturb this.
BEZEL_TEST(codegen_recursive_lambda_27_5, capture_normal_local_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var offset := 100; "
        "    var add: (int) -> int; "
        "    add := lambda(n: int) => n + offset; "  // captures `offset`, not `add`
        "    return add(23); "                 // 123
        "  end Main; "
        "end C;"), 123);
}

// §27.5: "and vice versa" — a mutation of a captured local AFTER the lambda is
// assigned is observed by the lambda when it later runs. The reorder must keep
// the shared-cell semantics intact for a captured (non-self) local.
BEZEL_TEST(codegen_recursive_lambda_27_5, outer_mutation_after_assign_seen) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Main(): int "
        "    var x := 1; "
        "    var f: () -> int; "
        "    f := lambda() => x; "         // captures x by reference
        "    x := 7; "              // written after the lambda is assigned
        "    return f(); "          // observes 7, not the assign-time 1
        "  end Main; "
        "end C;"), 7);
}
