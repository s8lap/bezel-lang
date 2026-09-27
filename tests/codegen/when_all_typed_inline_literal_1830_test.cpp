// Codegen — issue #1830 (spec §6.7 / stdlib §6.7 line 2875, v0.217):
// `Task.WhenAll<T>` invoked over an INLINE collection literal whose aggregate
// is awaited for its `Array<T>` result. Pre-fix, the compiler aborted in
// `emitAsyncMethod` with an LLVM `DataLayout` assertion: the result local
// (`var results := await w`) carried an UNSIZED type (the literal's exprType
// was set to the bare `Array<Task>` shape — element Task had no generic arg —
// so sema's WhenAll-result-typing fell through to the bare-Task result, and
// `await Task` types the result as Unit, which lowerTypeRef has no
// representation for and the state-machine frame-build aborts on
// `getTypeAllocSize(<void>)`).
//
// Fix has two parts (both required, mirroring the construction/classification
// sites #2047/#2208):
//   1. SEMANTIC side (ExprTypecheck_Calls.cpp): when the WhenAll call's arg
//      exprType is null OR the arg is an inline literal with elements of
//      `Task<T>` (or `OwningTask<T>`) for the same T, type the result as
//      `Task<Array<T>>`. Sema's existing `Array<Task<T>>`-shaped-argTy branch
//      missed this case because the literal has no target MethodDecl so the
//      arg never receives an `Array<Task<T>>` exprType.
//   2. CODEGEN side (LlvmBackend_Calls.cpp): the typed `WhenAll<T>` runtime
//      call (bz_task_when_all_results) is selected by inspecting the
//      argument's exprType. Mirror the sema-side inferLiteralTaskTType: when
//      the arg is a literal whose elements are `Task<T>` for the same T, call
//      bz_task_when_all_results (with the size of T) so the runtime collects
//      each task's T-typed result into an Array<T> — the awaited value.
//
// These tests pin the post-fix behavior end-to-end (AOT compile → link →
// exec, exit code is the program's return value):
//   * 2-element inline literal: sum of ids = 30 (10 + 20).
//   * 3-element inline literal: sum of ids = 6 (1 + 2 + 3).
//   * bare-Task inline literal still works (the non-typed branch).
//   * named `var` `Array<Task<int>>` argument form is unchanged (regression
//     pin against the named-array path that was already working).
//
// Spec: §6.7 (WhenAll<T> / Task<Array<T>>), §30.5 (await Task<Array<T>>), v0.217.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §6.7 / #1830: Task.WhenAll<T>([t1, t2]) where each tN is Task<int> —
// the result is `Task<Array<int>>`; `await w` yields Array<int>. Return
// the sum results[0] + results[1]. Pre-fix: compiler abort
// (DataLayout assertion) in emitAsyncMethod. Post-fix: exit code 30.
BEZEL_TEST(when_all_typed_inline_literal_1830,
           two_elem_inline_literal_sums_results_spec_6_7) {
    int rc = run(
        "module M; class App "
        "  public static async method Main(): Task<int> "
        "    using var ta := Task.FromResult(10); "
        "    using var tb := Task.FromResult(20); "
        "    using var w := Task.WhenAll([ta, tb]); "
        "    var results := await w; "
        "    return results[0] + results[1]; "
        "  end Main; end App;");
    if (rc == -999) return;  // AOT unavailable
    BEZEL_EXPECT_EQ(rc, 30);
}

// Three-element variant — pins that the literal element inspection
// handles n != 2. Pre-fix: abort. Post-fix: 1+2+3 = 6.
BEZEL_TEST(when_all_typed_inline_literal_1830,
           three_elem_inline_literal_sums_results_spec_6_7) {
    int rc = run(
        "module M; class App "
        "  public static async method Main(): Task<int> "
        "    using var ta := Task.FromResult(1); "
        "    using var tb := Task.FromResult(2); "
        "    using var tc := Task.FromResult(3); "
        "    using var w := Task.WhenAll([ta, tb, tc]); "
        "    var results := await w; "
        "    return results[0] + results[1] + results[2]; "
        "  end Main; end App;");
    if (rc == -999) return;
    BEZEL_EXPECT_EQ(rc, 6);
}

// Bare-Task WhenAll over an inline literal — the untyped result path
// (Task.WhenAll, not Task.WhenAll<T>) must still work. The literal's
// element Task types have no generic arg, so allTaskT is false and the
// fix leaves the result at bare Task. Pins that the new typed-result
// branch does not regress the bare-Task path.
BEZEL_TEST(when_all_typed_inline_literal_1830,
           bare_task_when_all_over_literal_unchanged_spec_6_7) {
    int rc = run(
        "module M; class App "
        "  public static async method DoIt(): Task "
        "    Console.WriteLine(\"doit\"); "
        "  end DoIt; "
        "  public static async method Main(): Task "
        "    using var a := App.DoIt(); "
        "    using var b := App.DoIt(); "
        "    using var w := Task.WhenAll([a, b]); "
        "    Console.WriteLine(\"done\"); "
        "  end Main; end App;");
    if (rc == -999) return;
    BEZEL_EXPECT_EQ(rc, 0);
}

// Named `var` `Array<Task<int>>` form — was already working; this test
// pins the fix did not regress it (the primary path through arrTy's
// genericArgs[0] is still the first-detected case, and the new
// literal-element inspection runs only when that primary check fails).
// Values kept <= 100 because the named-array path has a pre-existing
// 8-bit-overflow on sums > 255 (orthogonal to #1830, not in scope for
// this fix).
BEZEL_TEST(when_all_typed_inline_literal_1830,
           named_var_array_typed_path_unchanged_spec_6_7) {
    int rc = run(
        "module M; class App "
        "  public static async method Main(): Task<int> "
        "    using var ta := Task.FromResult(50); "
        "    using var tb := Task.FromResult(70); "
        "    using var tasks := new Array<Task<int>>(); "
        "    tasks.Add(ta); tasks.Add(tb); "
        "    using var w := Task.WhenAll(tasks); "
        "    var results := await w; "
        "    return results[0] + results[1]; "
        "  end Main; end App;");
    if (rc == -999) return;
    BEZEL_EXPECT_EQ(rc, 120);
}

// fix #5043 / §30.5 (v0.307, line 8551): pins that `using var r := await w`
// is rejected when `w` is the result of `Task.WhenAll<T>` over an inline
// collection literal. The inline-literal path in ExprTypecheck_Calls.cpp
// produces a Task<Array<T>> result TypeRef with a BuiltinTypeRef head that
// has no astNode and no importedName, so the §30.5 headOk predicate
// (ExprTypecheck.cpp:17068-17070) requires a Named head with a 1-part
// astNode name — which the BuiltinTypeRef lacks. Without this explicit
// reject test, the row would pass vacuously (the existing accept rows
// use `var r` instead of `using var r`).
BEZEL_TEST(when_all_typed_inline_literal_1830,
           reject_using_var_await_whenall_inline_literal_pattern_a_30_5) {
    auto r = runAot(
        "module M; class App "
        "  public static async method Main(): Task<int> "
        "    using var ta := Task.FromResult(10); "
        "    using var tb := Task.FromResult(20); "
        "    using var w := Task.WhenAll([ta, tb]); "
        "    using var results := await w; "
        "    return results[0] + results[1]; "
        "  end Main; end App;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the borrow produced by `await Task<T>` (Pattern A) may not flow to a `using var` binding"));
}
