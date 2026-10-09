// Regression test for #5441: a +1 string producer local passed as a borrowed
// `string` argument to a CLOSURE or IIFE leaked its whole scope-end release.
//
// THE DEFECT (measured on the staging tip 35759e44). The scope-end release for
// a +1 string producer local is armed only if `StringEscapeWalker` proves the
// local never escapes (`collectStringReleaseEligible`, LlvmBackend_Setup.cpp).
// An argument passed to a call escapes unless the walk's `safeArg` relaxation
// admits it. That relaxation was gated on `ms` — a resolvable
// `sema::MethodSymbol` — and a lambda / function-typed-local callee has NO
// MethodSymbol, so EVERY argument of EVERY closure call fell through to the
// conservative ESCAPE. A producer local passed to a closure was therefore
// excluded from `stringReleaseEligible_`: no scope-end release was armed, its
// +1 was never dropped, and the string leaked once per call.
//
// This is NOT the call-site pin bug #5441's original commit described (that one
// put the pointer in `closureStringPins` twice). That double-release cannot
// occur on this tree: `emitBorrowedStringArgPinsForClosure` has ZERO call
// sites (fix #5441/a92a20c4 removed its one call), so `allStringPins` is
// exactly `closureStringPins` and no pointer is ever pushed twice. The shapes
// the original commit targeted (direct +1 producers) are clean here; the
// producer-LOCAL shape is the one that leaks, and for a different reason.
//
// THE FIX. `walkCallArgs`'s `safeArg` relaxation gains its closure-callee
// twin: when there is no `ms`, read the formal list from the callee's own
// function `TypeRef` (`funcParams`) and admit a plain no-modifier position —
// the same information, and the same §28.3 "plain parameters are borrows"
// argument, for the call shape that carries no symbol table. Gated on
// `stringReleaseMode` alone so the borrowing-Array/Set/Dictionary walks that
// share this walker keep their conservative ESCAPE.
//
// SPEC: §28.3 "Plain parameters are borrows" / §28.9 "Runtime-managed types"
// (v0.330).
//
// FAIL-WITHOUT (measured, ASan/LSan, 500 iterations, looped):
//   named closure, producer-local arg   17500 B in 1000 allocations
//   IIFE, producer-local arg            17500 B in 1000 allocations
//   named closure, lambda ignores arg   17500 B in 1000 allocations
//   function-typed local invoke         17500 B in 1000 allocations
//   whole-matrix program                35000 B in 2000 allocations
// FAIL-WITH: LSan clean, exit 0, all rows.
//
// LOOP AMPLIFICATION: 500 iterations x 35 B = 17500 B direct + 17500 B indirect
// per row, far past the LSan noise floor. A one-shot repro is vacuous here.
//
// The `control_*` rows pin the method-callee and no-call twins, which were
// ALREADY clean before the fix (the #5275/#5300 admissions cover them). They
// are here to keep the fix from regressing a shape that never regressed: if a
// future change widened the relaxation too far, these rows are the ones that
// would catch a double-free.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Like runAotChecked but returns: 0=clean, 1=leak, -1=infra failure.
// A `!r.ran` with no "sanitizer fired" diagnostic is an INFRASTRUCTURE
// failure and returns -1 — it is NOT reported as a leak, and it is NOT
// silently treated as a pass either (see the callers below).
int checkLeak(const std::string& source) {
    auto r = runAotChecked(source);
    if (!r.ran) {
        auto& diags = r.diagnostics.diagnostics();
        for (auto& d : diags) {
            if (d.message.find("sanitizer fired") != std::string::npos)
                return 1;  // leak detected
        }
        return -1;  // some other infra issue
    }
    return 0;  // clean
}

// Returns exit code, or -1 if the program did not run at all.
int runExit(const std::string& source) {
    auto r = runAot(source);
    return r.ran ? r.exitCode : -1;
}

// ---------------------------------------------------------------------------
// PRIMARY ROWS — the producer-local shape that leaked, on both call shapes.
// ---------------------------------------------------------------------------

// Named closure: `var pl := "p" + "q"; f(pl);`
const std::string kNamedClosureProducerLocal =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    for var i in 0..<500 do "
    "      var f := lambda(s: string) => s.Length; "
    "      var pl := \"p\" + \"q\"; "
    "      var n := f(pl); "
    "      if n <> 2 then return 1; end; "
    "    end for; "
    "    return 0; "
    "  end Main; "
    "end C;";

// IIFE: `var pl := "p" + "q"; (lambda(s: string) => s.Length)(pl);`
const std::string kIIFEProducerLocal =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    for var i in 0..<500 do "
    "      var pl := \"p\" + \"q\"; "
    "      var n := (lambda(s: string) => s.Length)(pl); "
    "      if n <> 2 then return 1; end; "
    "    end for; "
    "    return 0; "
    "  end Main; "
    "end C;";

// The callee IGNORES its parameter — proving the leak does not depend on the
// lambda body reading the string. `q_ignore`, 17500 B before the fix.
const std::string kNamedClosureCalleeIgnores =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    for var i in 0..<500 do "
    "      var f := lambda(s: string) => 7; "
    "      var pl := \"p\" + \"q\"; "
    "      var n := f(pl); "
    "      if n <> 7 then return 1; end; "
    "    end for; "
    "    return 0; "
    "  end Main; "
    "end C;";

// Function-TYPED LOCAL invoke (`var f: (string) -> int := lambda...; f(pl)`) —
// the third callee spelling with no MethodSymbol. `t2_fnlocal`.
const std::string kFnTypedLocalInvoke =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    for var i in 0..<500 do "
    "      var f: (string) -> int := lambda(s: string) => s.Length; "
    "      var pl := \"p\" + \"q\"; "
    "      var n := f(pl); "
    "      if n <> 2 then return 1; end; "
    "    end for; "
    "    return 0; "
    "  end Main; "
    "end C;";

// Whole matrix: all four argument shapes x both call shapes in one program.
// Looped 500x, so a single surviving leak shape is unambiguous. Before the
// fix: 35000 B in 2000 allocations (the two producer-local rows).
const std::string kAllShapesMatrix =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    for var i in 0..<500 do "
    "      var f := lambda(s: string) => s.Length; "
    "      if f(\"ab\") <> 2 then return 1; end; "
    "      if f(\"a\" + \"b\") <> 2 then return 2; end; "
    "      if f($\"a{i}\") < 2 then return 3; end; "
    "      var pl := \"p\" + \"q\"; "
    "      if f(pl) <> 2 then return 4; end; "
    "      var oth: string := \"xy\"; "
    "      if f(oth) <> 2 then return 5; end; "
    "      if (lambda(s: string) => s.Length)(\"ab\") <> 2 then return 6; end; "
    "      if (lambda(s: string) => s.Length)(\"a\" + \"b\") <> 2 then return 7; end; "
    "      if (lambda(s: string) => s.Length)($\"a{i}\") < 2 then return 8; end; "
    "      var ipl := \"p\" + \"q\"; "
    "      if (lambda(s: string) => s.Length)(ipl) <> 2 then return 9; end; "
    "      var ioth: string := \"xy\"; "
    "      if (lambda(s: string) => s.Length)(ioth) <> 2 then return 10; end; "
    "    end for; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---------------------------------------------------------------------------
// CONTROL ROWS — clean BEFORE the fix; kept so a widened relaxation that
// over-releases (double-free) is caught rather than shipped.
// ---------------------------------------------------------------------------

// Method callee with a plain `string` formal — the #1430 admission that
// already worked (`plainBorrowAnalysisCallee` returns a MethodSymbol).
const std::string kControlMethodCallee =
    "module M; "
    "class D "
    "  public method Take(s: string): int "
    "    return s.Length; "
    "  end Take; "
    "end D; "
    "class C "
    "  public static method Main(): int "
    "    var d := new D(); "
    "    for var i in 0..<500 do "
    "      var pl := \"p\" + \"q\"; "
    "      var n := d.Take(pl); "
    "      if n <> 2 then return 1; end; "
    "    end for; "
    "    return 0; "
    "  end Main; "
    "end C;";

// Generic method callee — the #5300 bare-type-parameter-return admission.
const std::string kControlGenericCallee =
    "module M; "
    "class D "
    "  public method Id<T>(v: T): T "
    "    return v; "
    "  end Id; "
    "end D; "
    "class C "
    "  public static method Main(): int "
    "    var d := new D(); "
    "    for var i in 0..<500 do "
    "      var pl := \"p\" + \"q\"; "
    "      var n := d.Id<string>(pl).Length; "
    "      if n <> 2 then return 1; end; "
    "    end for; "
    "    return 0; "
    "  end Main; "
    "end C;";

// Producer local never arg-passed — its scope-end release must still fire
// (the plain-`:=` safe-read relaxation, #1997).
const std::string kControlNoArgPass =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    for var i in 0..<500 do "
    "      var pl := \"p\" + \"q\"; "
    "      var n := pl.Length; "
    "      if n <> 2 then return 1; end; "
    "    end for; "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// ===========================================================================
// PRIMARY ROWS. Each: looped 500x, exit code AND LSan cleanliness.
// A leak is `checkLeak() == 1`; an infrastructure failure is `-1` and is
// reported as such rather than silently passing.
// ===========================================================================
BEZEL_TEST(closure_arg_producer_local_scope_release_5441,
           named_closure_producer_local_28_3_28_9) {
    int exitCode = runExit(kNamedClosureProducerLocal);
    BEZEL_EXPECT_EQ(exitCode, 0);
    int leak = checkLeak(kNamedClosureProducerLocal);
    if (leak == -1) {
        std::fprintf(stderr,
                     "5441 named_closure_producer_local: AOT leg did not run\n");
        BEZEL_EXPECT_EQ(-1, 0);
        return;
    }
    BEZEL_EXPECT_EQ(leak, 0);  // 0=clean, 1=leak
}

BEZEL_TEST(closure_arg_producer_local_scope_release_5441,
           iife_producer_local_28_3_28_9) {
    int exitCode = runExit(kIIFEProducerLocal);
    BEZEL_EXPECT_EQ(exitCode, 0);
    int leak = checkLeak(kIIFEProducerLocal);
    if (leak == -1) {
        std::fprintf(stderr,
                     "5441 iife_producer_local: AOT leg did not run\n");
        BEZEL_EXPECT_EQ(-1, 0);
        return;
    }
    BEZEL_EXPECT_EQ(leak, 0);
}

BEZEL_TEST(closure_arg_producer_local_scope_release_5441,
           named_closure_callee_ignores_arg_28_3) {
    int exitCode = runExit(kNamedClosureCalleeIgnores);
    BEZEL_EXPECT_EQ(exitCode, 0);
    int leak = checkLeak(kNamedClosureCalleeIgnores);
    if (leak == -1) {
        std::fprintf(stderr,
                     "5441 named_closure_callee_ignores: AOT leg did not run\n");
        BEZEL_EXPECT_EQ(-1, 0);
        return;
    }
    BEZEL_EXPECT_EQ(leak, 0);
}

BEZEL_TEST(closure_arg_producer_local_scope_release_5441,
           fn_typed_local_invoke_producer_local_28_3) {
    int exitCode = runExit(kFnTypedLocalInvoke);
    BEZEL_EXPECT_EQ(exitCode, 0);
    int leak = checkLeak(kFnTypedLocalInvoke);
    if (leak == -1) {
        std::fprintf(stderr,
                     "5441 fn_typed_local_invoke: AOT leg did not run\n");
        BEZEL_EXPECT_EQ(-1, 0);
        return;
    }
    BEZEL_EXPECT_EQ(leak, 0);
}

BEZEL_TEST(closure_arg_producer_local_scope_release_5441,
           all_shapes_both_call_shapes_28_3) {
    int exitCode = runExit(kAllShapesMatrix);
    BEZEL_EXPECT_EQ(exitCode, 0);
    int leak = checkLeak(kAllShapesMatrix);
    if (leak == -1) {
        std::fprintf(stderr, "5441 all_shapes_matrix: AOT leg did not run\n");
        BEZEL_EXPECT_EQ(-1, 0);
        return;
    }
    BEZEL_EXPECT_EQ(leak, 0);
}

// ===========================================================================
// CONTROL ROWS — clean before AND after the fix. A double-free here means the
// relaxation was widened past the shapes it may admit.
// ===========================================================================
BEZEL_TEST(closure_arg_producer_local_scope_release_5441,
           control_method_callee_plain_formal_28_3) {
    int exitCode = runExit(kControlMethodCallee);
    BEZEL_EXPECT_EQ(exitCode, 0);
    int leak = checkLeak(kControlMethodCallee);
    if (leak == -1) {
        std::fprintf(stderr, "5441 control_method_callee: AOT leg did not run\n");
        BEZEL_EXPECT_EQ(-1, 0);
        return;
    }
    BEZEL_EXPECT_EQ(leak, 0);
}

BEZEL_TEST(closure_arg_producer_local_scope_release_5441,
           control_generic_callee_plain_formal_28_3) {
    int exitCode = runExit(kControlGenericCallee);
    BEZEL_EXPECT_EQ(exitCode, 0);
    int leak = checkLeak(kControlGenericCallee);
    if (leak == -1) {
        std::fprintf(stderr,
                     "5441 control_generic_callee: AOT leg did not run\n");
        BEZEL_EXPECT_EQ(-1, 0);
        return;
    }
    BEZEL_EXPECT_EQ(leak, 0);
}

BEZEL_TEST(closure_arg_producer_local_scope_release_5441,
           control_producer_local_never_arg_passed_28_3) {
    int exitCode = runExit(kControlNoArgPass);
    BEZEL_EXPECT_EQ(exitCode, 0);
    int leak = checkLeak(kControlNoArgPass);
    if (leak == -1) {
        std::fprintf(stderr, "5441 control_no_arg_pass: AOT leg did not run\n");
        BEZEL_EXPECT_EQ(-1, 0);
        return;
    }
    BEZEL_EXPECT_EQ(leak, 0);
}