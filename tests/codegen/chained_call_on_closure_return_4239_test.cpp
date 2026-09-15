// Codegen — #4239 (spec §28.3 / §30.5): chained call on a returned closure
// temporary — `Foo()()` — double-frees the temporary.
//
// Bug: `return SomeMethodReturningAClosure()();` — calling the closure a
// method returns IMMEDIATELY, without first binding it to a local — double-
// frees the temporary closure value. Reproduces at ONE level of lambda nesting.
// Both the double-free trigger and the "previously freed" site are attributed
// to `main` itself (not to any lambda's envdtor) — pointing at `main`'s
// own generated code for the `CallExpr(CallExpr(...))` chain.
//
// The temporary returned by the inner call needs exactly ONE "drop after call"
// disposal, but the code path added it twice.
//
// Spec authority:
//   - §28.3 (Unbound owned temporaries): "Such a temporary is disposed at
//     the end of the full expression in which it is produced, after every
//     use of it within that expression has completed."
//   - §30.5 (Closure returns): when the inner method returns a +1 producer
//     (lambda literal, method ref), the return does NOT mint; closure arrives
//     with rc=1; ONE release balances it. When it returns a borrowed value
//     (a call to a lambda-typed local), the return MINTS; closure arrives
//     with rc=2; TWO releases balance it.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

// Issue #4239 exact repro (verbatim from issue, ASan-confirmed double-free).
// The closure returned by MakeClosure captures a local `f`; the inner call
// `reader()` uses `f` before `f` is returned. The outer `MakeClosure()()`
// chain-call is the double-free trigger.
const char* kChainedCallOnClosureReturn = R"(
module M;
class C
  public static method MakeClosure(): () -> int
    var f: () -> int := lambda() => 7;
    var reader := lambda() => do
      var x := f();
    end;
    reader();
    return f;
  end MakeClosure;
  public static method Main(): int
    return C.MakeClosure()();
  end Main;
end C;
)";

namespace {

// Run the program and return the exit code, or -999 on compile/run failure.
// ASan double-free detection causes the subprocess to abort (exit code -6
// or SIGABRT), which runAotChecked propagates as a non-zero exit code.
int run(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// #4239: chained call on closure-returning method — no double-free.
BEZEL_TEST(codegen_chained_closure_return_4239,
          chained_call_on_closure_return_4239_spec_28_3) {
    // Expected: exit code 7 (the closure returns 7).
    // Without the fix: ASan aborts with "free(): invalid pointer" or
    // "double-free" in main, before the return value is even observed.
    BEZEL_EXPECT_EQ(run(kChainedCallOnClosureReturn), 7);
}

// Variant: binding the closure to a local first (the workaround) — should
// also work and is included as a sanity check that the scenario is valid.
BEZEL_TEST(codegen_chained_closure_return_4239,
          bound_local_then_call_4239_sanity) {
    const char* src = R"(
module M;
class C
  public static method MakeClosure(): () -> int
    var f: () -> int := lambda() => 7;
    return f;
  end MakeClosure;
  public static method Main(): int
    var fn := C.MakeClosure();
    return fn();
  end Main;
end C;
)";
    BEZEL_EXPECT_EQ(run(src), 7);
}