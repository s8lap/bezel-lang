// Codegen — issue #3402 (§21.1 runtime `const` locals, §27.5 closures,
// §27.6 method references, §28.9 runtime-managed types, spec v0.283): a
// §21.1 runtime `const` local bound to a lambda never freed the closure cell
// codegen allocated for it. Measured on the pre-fix compiler:
//
//     const f := lambda(x: int) => x + 7;   ->  40 bytes / 1 allocation leaked
//     var k: int := 7;
//     const f := lambda(x: int) => x + k;   ->  48 bytes / 2 allocations
//     const f := C.Helper;                  ->  40 bytes / 1 allocation
//
// while the `var` spelling of every one of them was ASan-clean, in a plain
// SYNCHRONOUS method with no async machinery and no iterator involved.
//
// ROOT CAUSE — the THIRD instance of one pattern. §21.1 gives a runtime
// `const` its own AST node: `LocalConstDecl : Stmt`, a sibling of
// `LocalDecl : Stmt` with differently-named fields (`value` where a `var` has
// `initializer`) and no `isUsing` at all. A codegen walker written as
// `dynamic_cast<const ast::LocalDecl*>(s)` therefore matches a `var` and
// silently does nothing for a `const`. #2802 found it in the collection-
// literal reclaim scan; #3375 found it in `collectSuspendLocals`, where it
// meant no `const` local ever received a state-machine frame field; #3402 is
// the same shape in the CLOSURE reclaim family. Four walkers were affected:
//
//   1. collectClosureReleaseEligible — the candidate scan matched LocalDecl
//      only, so a `const`-bound lambda / method reference never entered
//      `closureReleaseEligible_` and `lowerLocalDecl` never armed its
//      scope-end `bz_closure_release`. This is the issue's 40-byte leak.
//      (No consumer change was needed: `lowerStmt` routes the `const` arm
//      through the SAME `lowerLocalDecl`, whose arming reads the eligibility
//      set by NODE POINTER — `const void*` — never by node kind.)
//
//   2. collectCellFreeEligible's LambdaFinder::stmt — no LocalConstDecl arm
//      at all, so it never even DESCENDED into a `const`'s initializer. A
//      lambda written there was invisible to BOTH classifications: its
//      captures never reached `capturedByEscaping`, so a sibling non-escaping
//      `var` lambda over the same local still armed the scope-end capture-cell
//      free. That is a HEAP-USE-AFTER-FREE, not a leak, and it reproduced (see
//      the escaping_const_lambda_… test below; pre-fix ASan reported
//      "heap-use-after-free … READ of size 4" inside the returned closure).
//
//   3. returnedClosureLocalOwnsPlusOne — read the declaration through a
//      LocalDecl-only cast, so `return f;` for a `const`-bound closure fell
//      through to the retain path and minted a SECOND reference; the cell's
//      count never reached 0 (52 bytes / 3 allocations, `var` twin clean).
//
//   4. collectDefinitelyEscapingLambdas — its `returnedVars` candidates were
//      LocalDecl-only, so a returned `const`-bound lambda was never marked
//      definitely-escaping and its envdtor did not free the capture cell it
//      solely holds.
//
// The fix widens all four to read the declaration through ONE shared
// authority — `Emitter::localDeclInitView`, the `LocalDeclInitView` split
// #2802 introduced, hoisted from a function-local lambda to a member so a
// fifth private copy of the predicate is not what closes the fifth instance.
//
// WHY IT IS NOT A DOUBLE-FREE (Appendix C). The fix adds no §28.3 disposal
// obligation and creates no second owner. §28.9 makes a function-typed value
// RUNTIME-MANAGED — "Function-typed values … carry no disposal obligation" —
// and §21.1's *Ownership* paragraph admits exactly such values as a runtime
// `const`'s initializer ("`string` and other runtime-managed values"); what a
// `const` may NOT bind is an owned production, and a lambda is not one. What
// is released is codegen's OWN +1 on the closure cell, minted by lowerLambda /
// makeMethodThunk and transferred into the slot — the same single reference
// the `var` path has always released, at the same point, under the same
// conservative escape walk. Exactly-once is in fact EASIER to establish for a
// `const` than for a `var`: §21.1 makes a `const` "not an assignable
// location", so the drop-on-overwrite edge a `var` must reason about cannot be
// spelled at all, and the binding is initialized exactly once by construction.
//
// Every test below uses runAotChecked (generated-code ASan with LeakSanitizer
// on), so `r.ran` is itself the leak assertion — with ONE deliberate
// exception, the memory-safety test, which asserts a wrong-value symptom under
// plain `runAot` for the reasons given at its own comment. The paired `var`
// tests are controls: they passed before and must keep passing, which is what
// makes each `const`/`var` pair a parity assertion rather than a bare leak
// check.
//
// FAIL-WITHOUT / PASS-WITH, measured program-by-program on one build of each
// compiler (`bezelc --sanitize=address`, LeakSanitizer on):
//
//   program                                   pre-fix              post-fix
//   const f := lambda(x) => x + 7             40 B / 1 alloc       clean
//   var  f := lambda(x) => x + 7  (control)   clean                clean
//   var k; const f := lambda(x) => x + k      48 B / 2 allocs      clean
//   const k; var f := lambda(x) => x + k      clean                clean
//   const f := C.Helper           (§27.6)     40 B / 1 alloc       clean
//   const f := lambda() => k; return f        52 B / 3 allocs      clean
//   var  f := lambda() => k; return f         clean                clean
//   const lambda in a 2000-iteration loop     80000 B / 2000       clean
//   const lambda in an if / do…end block      40 B / 1 alloc       clean
//   const f := C.Make()           (#429)      40 B / 1 alloc       clean
//   const n := p.Length + 6       (guard)     clean                clean
//   escaping const lambda + sibling var       heap-use-after-free  h=7 (was 99)
// per §21.1, §27.5, §27.6, §28.9 (issue #3402)

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
std::string runCheckedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ── The issue's exact repro: a non-capturing lambda bound to a §21.1 runtime
// `const`. Pre-fix: 40 bytes / 1 allocation leaked (the calloc'd rc=1 closure
// cell). The value 10 also has to be right — the release must not disturb the
// call. // per §21.1, §27.5, §28.9 (issue #3402)
BEZEL_TEST(const_local_closure_release_3402,
           const_local_bound_lambda_cell_released_spec_21_1_27_5_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    const f := lambda(x: int) => x + 7; "
        "    Console.WriteLine(\"f=\" + f(3).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "f=10\n");
}

// ── PARITY CONTROL for the test above: the `var` spelling was already clean
// pre-fix and must stay clean. The pair is the assertion — the two spellings
// must behave identically. // per §21.1, §27.5 (issue #3402)
BEZEL_TEST(const_local_closure_release_3402,
           var_local_bound_lambda_cell_released_parity_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var f := lambda(x: int) => x + 7; "
        "    Console.WriteLine(\"f=\" + f(3).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "f=10\n");
}

// ── CAPTURE axis: the issue's grid row `var k; const f := lambda … + k`.
// Pre-fix: 48 bytes / 2 allocations (the closure cell plus the capture cell).
// The leaking binding is the one holding the LAMBDA, so this widens the
// footprint rather than changing which decl is at fault.
// per §21.1, §27.5, §28.9 (issue #3402)
BEZEL_TEST(const_local_closure_release_3402,
           const_local_capturing_lambda_cell_and_capture_released_spec_27_5) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var k: int := 7; "
        "    const f := lambda(x: int) => x + k; "
        "    Console.WriteLine(\"f=\" + f(3).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "f=10\n");
}

// ── The issue's control row: whether the CAPTURED local is `var` or `const`
// is irrelevant; only the binding that holds the lambda matters. This is the
// clean-before-and-after direction, pinned so a future change cannot start
// leaking it. // per §21.1, §27.5 (issue #3402)
BEZEL_TEST(const_local_closure_release_3402,
           const_captured_local_with_var_lambda_binding_clean_spec_27_5) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    const k: int := 7; "
        "    var f := lambda(x: int) => x + k; "
        "    Console.WriteLine(\"f=\" + f(3).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "f=10\n");
}

// ── §27.6 METHOD REFERENCE axis: `const f := C.Helper;` lowers to the same
// fresh rc=1 cell via makeMethodThunk, and leaked identically pre-fix
// (40 bytes / 1 allocation). isPlusOneClosureProducer already recognized the
// shape; it was the LocalDecl-only SCAN that dropped it.
// per §21.1, §27.6, §28.9 (issue #3402)
BEZEL_TEST(const_local_closure_release_3402,
           const_local_bound_method_reference_cell_released_spec_21_1_27_6) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Helper(x: int): int "
        "    return x + 7; "
        "  end Helper; "
        "  public static method Main(): int "
        "    const f := C.Helper; "
        "    Console.WriteLine(\"f=\" + f(3).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "f=10\n");
}

// ── MEMORY-SAFETY axis (walker 2 above). A `const`-bound lambda capturing the
// same local as a non-escaping `var`-bound one, then returned. Pre-fix
// LambdaFinder::stmt never descended into the `const` initializer, so the
// `const` lambda's capture of `k` was never recorded as escaping, the sibling
// `var` lambda still armed the scope-end capture-cell free, and calling the
// returned closure read `k` through freed memory. Measured on the pre-fix
// compiler under ASan: "ERROR: AddressSanitizer: heap-use-after-free … READ of
// size 4" inside the returned closure's body.
//
// TWO DELIBERATE DEVIATIONS from the rest of this file, both forced by that
// shape and both making the test STRONGER, not weaker:
//
//   * It uses `runAot`, NOT `runAotChecked`. The "one escaping plus one
//     non-escaping lambda over the same capture" shape leaks the 4-byte
//     capture cell — an UNRELATED, PRE-EXISTING defect that the all-`var`
//     spelling of this same program exhibits identically (measured: 4 bytes /
//     1 allocation, before and after this fix, `var` and `const` alike). A
//     LeakSanitizer assertion here would therefore fail for a reason that has
//     nothing to do with #3402.
//
//   * The observable is a WRONG VALUE, not a sanitizer verdict — which is why
//     dropping ASan costs nothing. `Main` allocates its own int capture cell
//     (`m`/`q`) immediately after `Make` returns; the allocator hands back the
//     block `Make` just freed, so the pre-fix build reads Main's 99 through
//     the dangling pointer. Measured on a PLAIN (unsanitized) build:
//     pre-fix prints "h=99", post-fix prints "h=7". A use-after-free that only
//     ASan can see would have made a vacuous test; this one cannot pass by
//     luck.
// per §21.1, §27.5, §28.9 (issue #3402)
BEZEL_TEST(const_local_closure_release_3402,
           escaping_const_lambda_capture_not_freed_early_spec_27_5_28_9) {
    auto r = runAot(
        "module M; import System; "
        "class C "
        "  public static method Make(): () -> int "
        "    var k: int := 7; "
        "    var g := lambda() => k; "
        "    Console.WriteLine(\"g=\" + g().ToString()); "
        "    const f := lambda() => k; "
        "    return f; "
        "  end Make; "
        "  public static method Main(): int "
        "    var h := C.Make(); "
        "    var m: int := 99; "
        "    var q := lambda() => m; "
        "    Console.WriteLine(\"q=\" + q().ToString()); "
        "    Console.WriteLine(\"h=\" + h().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    // "h=7", not the pre-fix "h=99" read through the freed capture cell.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("g=7\nq=99\nh=7\n"));
}

// ── RETURN-TRANSFER axis (walkers 3 and 4 above): `const f := lambda …;
// return f;`. Pre-fix 52 bytes / 3 allocations — the return minted a second
// reference so the cell never reached rc 0, and the capture cell was never
// freed by the envdtor either. The `var` twin was clean; this is the parity
// the fix restores. // per §21.1, §27.5, §28.9 (issue #3402)
BEZEL_TEST(const_local_closure_release_3402,
           returned_const_local_closure_transfers_plus_one_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Make(): () -> int "
        "    var k: int := 7; "
        "    const f := lambda() => k; "
        "    return f; "
        "  end Make; "
        "  public static method Main(): int "
        "    var h := C.Make(); "
        "    Console.WriteLine(\"h=\" + h().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "h=7\n");
}

// ── PARITY CONTROL for the return-transfer axis.
// per §27.5, §28.9 (issue #3402)
BEZEL_TEST(const_local_closure_release_3402,
           returned_var_local_closure_transfers_plus_one_parity_spec_27_5) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Make(): () -> int "
        "    var k: int := 7; "
        "    var f := lambda() => k; "
        "    return f; "
        "  end Make; "
        "  public static method Main(): int "
        "    var h := C.Make(); "
        "    Console.WriteLine(\"h=\" + h().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "h=7\n");
}

// ── UNBOUNDED axis: the leak is per-execution, so a loop turns it from a
// bounded 40 bytes into 40 x N. 2000 iterations = 80 KB pre-fix, far above
// LeakSanitizer's reporting floor and the reason the issue rates the defect
// MEDIUM rather than cosmetic. // per §21.1, §27.5, §28.9 (issue #3402)
BEZEL_TEST(const_local_closure_release_3402,
           const_local_lambda_in_loop_body_reclaimed_each_iter_spec_27_5) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..2000 do "
        "      const f := lambda(x: int) => x + 1; "
        "      t := t + f(i); "
        "    end; "
        "    return t mod 7; "
        "  end Main; "
        "end C;"),
        /* sum(2..2001) = 2003000; 2003000 mod 7 = 6 */ 6);
}

// ── SCAN-RECURSION axis: the candidate scan descends through the compound
// statements, so a `const`-bound lambda nested in an `if` branch and one in a
// `do … end` block must both be reclaimed — a widening that only fixed the
// top level would leave these leaking.
// per §21.1, §21.2, §21.6, §27.5 (issue #3402)
BEZEL_TEST(const_local_closure_release_3402,
           const_local_lambda_in_nested_blocks_reclaimed_spec_21_2_21_6) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var t := 0; "
        "    if t = 0 then "
        "      const f := lambda(x: int) => x + 3; "
        "      t := t + f(1); "
        "    end; "
        "    do "
        "      const g := lambda(x: int) => x + 5; "
        "      t := t + g(1); "
        "    end; "
        "    Console.WriteLine(\"t=\" + t.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "t=10\n");
}

// ── #429 RETURN-CATEGORY axis: a `const` bound to a call of a user-defined
// method whose declared return type is a function type. That callee mints
// exactly +1 on return, so the `const` local owns that single reference and
// must release it at scope end — the second half of the candidate predicate
// (isUserMethodClosureReturnCall), which the LocalDecl-only scan dropped for
// a `const` along with the lambda half.
// per §21.1, §27.1, §27.5, §28.9 (issue #3402)
BEZEL_TEST(const_local_closure_release_3402,
           const_local_bound_closure_returning_call_released_spec_27_1_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Make(): (int) -> int "
        "    return lambda(x: int) => x + 7; "
        "  end Make; "
        "  public static method Main(): int "
        "    const f := C.Make(); "
        "    Console.WriteLine(\"f=\" + f(3).ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "f=10\n");
}

// ── §17.15 / §21.1 adjacency guard: a `const` whose initializer is NOT a
// closure producer must be unaffected by the widening — the candidate
// predicate still gates on isPlusOneClosureProducer /
// isUserMethodClosureReturnCall, so admitting the `const` NODE must not arm a
// closure release for a `const` that holds no closure. A spurious release here
// would be a wild free, not a leak, so this pins the negative direction.
// per §21.1, §28.9 (issue #3402)
BEZEL_TEST(const_local_closure_release_3402,
           non_closure_const_local_unaffected_by_widening_spec_21_1) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var p := \"ab\"; "
        "    const n := p.Length + 6; "
        "    const b := n > 3; "
        "    Console.WriteLine(\"n=\" + n.ToString() + \",b=\" + b.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "n=8,b=True\n");
}
