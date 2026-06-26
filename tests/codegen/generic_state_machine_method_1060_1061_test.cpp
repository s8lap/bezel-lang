// Codegen — a GENERIC async / iterator method (a state-machine method with
// type parameters) is monomorphized with its state-machine transformation
// applied to the specialized function (issues #1060 / #1061; spec §14.1 / §29 /
// §30).
//
// Root cause (before fix): a generic method is monomorphized per call site by
// buildSpecializedMethodFn (LlvmBackend_Calls.cpp), which emitted the
// specialized body PLAINLY — a raw `lowerScopedBlock` / `lowerExpr` — and never
// applied the async / iterator state-machine TRANSFORMATION that the
// non-specialized method emission (LlvmBackend_Setup.cpp emitFunctionBody) routes
// through. So:
//   - a generic ASYNC method `async method Echo<T>(v: T): Task<T> return v`
//     emitted `ret <T>` directly into a Task-returning function → LLVM IR
//     verification failure ("Function return type does not match operand type of
//     return inst") — issue #1061.
//   - a generic ITERATOR method `method Gen<T>(...): IEnumerable<int>` with
//     `yield return` emitted a yield body with no state machine → SIGSEGV when
//     consumed — issue #1060.
//
// After fix: buildSpecializedMethodFn detects a state-machine method (the same
// containsYield / isAsyncMethod / returnsAsyncIteratorType predicates the
// non-specialized path uses) and routes the specialized function through
// emitAsyncMethod / emitIteratorMethod / emitAsyncIteratorMethod with the active
// generic substitution, so the specialized state machine matches the specialized
// signature. The full state-machine emission context is saved/restored across the
// (lazy) specialization so building it mid-emission of another method is
// transparent.
//
// Spec references:
//   §14.1 — generic-method monomorphization; the return type is the substituted T.
//   §29   — iterator methods (`yield return` → IEnumerable<T> state machine).
//   §30.1 — async methods (`async … : Task<T>` → Task-returning state machine).

#include <string>

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// #1061: a generic async method, consumed directly via `.Result`, returns its
// value (no IR verification failure).
BEZEL_TEST(generic_state_machine_method_1060_1061, generic_async_result) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static async method Echo<T>(v: T): Task<T> return v; end Echo; "
        "  public static method Main(): int "
        "    var t: Task<int> := Echo<int>(42); return t.Result; "
        "  end Main; end C;"), 42);
}

// #1061: a generic async method whose type parameter is NOT in the signature
// (Task<int>); the specialization is still distinct and runs.
BEZEL_TEST(generic_state_machine_method_1060_1061, generic_async_unused_typeparam) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static async method Echo<T>(n: int): Task<int> return n + 1; end Echo; "
        "  public static method Main(): int "
        "    var t: Task<int> := Echo<bool>(41); return t.Result; "
        "  end Main; end C;"), 42);
}

// #1060: a generic iterator method (IEnumerable<int>) consumed via for-in sums
// its yielded values (no SIGSEGV).
BEZEL_TEST(generic_state_machine_method_1060_1061, generic_iterator_for_in) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Gen<T>(n: int): IEnumerable<int> "
        "    for var i in 1..n do yield return i; end; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var s: int := 0; "
        "    for var x in Gen<bool>(5) do s := s + x; end; "
        "    return s; "
        "  end Main; end C;"), 15);
}

// #1060: a generic iterator method that yields its type parameter T.
BEZEL_TEST(generic_state_machine_method_1060_1061, generic_iterator_yields_typeparam) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Repeat<T>(v: T, n: int): IEnumerable<T> "
        "    for var i in 0..<n do yield return v; end; "
        "  end Repeat; "
        "  public static method Main(): int "
        "    var s: int := 0; "
        "    for var x in Repeat<int>(7, 3) do s := s + x; end; "
        "    return s; "
        "  end Main; end C;"), 21);
}

// REGRESSION: a NON-generic async method still works (the routing must not
// disturb the non-specialized state-machine emission).
BEZEL_TEST(generic_state_machine_method_1060_1061, nongeneric_async_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static async method Echo(v: int): Task<int> return v + 100; end Echo; "
        "  public static method Main(): int "
        "    var t: Task<int> := Echo(11); return t.Result; "
        "  end Main; end C;"), 111);
}

// REGRESSION: a NON-generic iterator method still works.
BEZEL_TEST(generic_state_machine_method_1060_1061, nongeneric_iterator_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Gen(n: int): IEnumerable<int> "
        "    for var i in 1..n do yield return i * 2; end; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var s: int := 0; "
        "    for var x in Gen(4) do s := s + x; end; "
        "    return s; "
        "  end Main; end C;"), 20);
}

// #1062: consuming a generic state-machine specialization INSIDE another state
// machine. The specialization is built LAZILY, mid-emission of the outer
// machine's body; emitResumeDispatch runs its done-block callback
// (→ emitAsyncComplete) before publishing smStateStruct_/smSelf_, so without
// resetting those scalars to their fresh (null) top-level values the spec's own
// mn.done emitted a GEP/free of the OUTER frame (%async.Run / the outer resume's
// arg) — a cross-function reference ("Referring to an argument in another
// function") IR-verification failure. buildSpecializedMethodFn now clears the SM
// scalars before routing to the helper, so the spec starts like a top-level
// method.

// Awaiting a generic async method inside another async method.
BEZEL_TEST(generic_state_machine_method_1060_1061, await_generic_async_in_async_1062) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static async method Echo<T>(v: T): Task<T> return v; end Echo; "
        "  public static async method Run(): Task<int> "
        "    var a: int := await Echo<int>(20); "
        "    var b: int := await Echo<int>(22); "
        "    return a + b; "
        "  end Run; "
        "  public static method Main(): int return Run().Result; end Main; end C;"),
        42);
}

// Consuming a generic iterator inside another (non-generic) iterator.
BEZEL_TEST(generic_state_machine_method_1060_1061, generic_iterator_in_iterator_1062) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Inner<T>(n: int): IEnumerable<int> "
        "    for var i in 1..n do yield return i; end; "
        "  end Inner; "
        "  public static method Outer(): IEnumerable<int> "
        "    for var x in Inner<bool>(3) do yield return x * 10; end; "
        "  end Outer; "
        "  public static method Main(): int "
        "    var s: int := 0; "
        "    for var y in Outer() do s := s + y; end; "
        "    return s; "
        "  end Main; end C;"), 60);
}
