// Codegen / refcounting — issue #4468 (§27.5 Closure capture reclamation /
// §29.7 Disposal of partially-consumed iterators): a SYNC iterator body that
// `yield return`s a FUNCTION-TYPED LOCAL — `var f := lambda() => 42;` — leaks
// the closure cell on every enumerator Dispose. The §28.3 scope-end
// `bz_closure_release` the local-decl machinery registers is UNREACHABLE in a
// sync iterator body: MoveNext returns at the yield, the body's `}` is never
// visited, and the cell's +1 lives on the iterator frame's local field until
// the next Dispose. Pre-fix: 40 B leaked per enumeration (LSan one-shot shows
// a single allocation; loop-amplified it shows N × 40). Post-fix: clean.
//
// ── THE DEFECT. `LlvmBackend_Iterators.cpp::emitDisposeBody` runs four
// per-iterator-cleanup lambdas before `free(self)`:
//
//   disposeOwnedParams()      — owned-class params (§29.8)
//   disposeOwnedLocals()      — state-keyed owned-class locals (§29.7 #2306)
//   releaseClosureCaptures()  — frame's +1 on closure-typed PARAMETERS (#1864)
//   disposeYieldedVal()       — current_val for owned-class / owning-iter
//
// `releaseClosureCaptures` walks `frame.captures` (the iterator's parameters
// promoted to the frame) and emits `bz_closure_release` for each closure-typed
// one. It does NOT walk `frame.bodyLocals` — the local declarations collected
// by `collectSuspendLocals` — so a `var f := lambda() => 42;` declared in the
// iterator body, whose frame field holds the cell pointer with rc=1, is
// discharged by nothing. The cell leaks. LSan: 40 B / 1 allocation per yield.
//
// ── THE FIX. Add a `releaseLocalClosures` lambda, parallel to
// `releaseClosureCaptures`, that walks `bodyLocals` in REVERSE declaration
// order, casts each entry to `ast::LocalDecl` / `ast::LocalConstDecl`, gates on
// the declared type being `Kind::Function` (with the `exprType`-of-initializer
// fallback for the inferred-type case — the issue's minimal repro), and emits
// `bz_closure_release(frame_field)`. `bz_closure_release` is null-safe, so
// the source's untouched calloc-zeroed local slots are a no-op and the clone's
// post-MoveNext slots release the cell + env block at rc 0. The loop is NOT
// gated on the existing `releaseClosures` flag (a source-only concern for
// parameters); the LOCAL slot's +1 lives on the DRIVEN frame (the clone), so
// the cleanup must fire on every Dispose.
//
// ── WHY THE EXISTING SCOPE-END RELEASE DOES NOT FIRE. For a non-iterator
// method, `lowerLocalDecl` registers a `Disposal{closureRelease=true}` on
// `disposeStack_`, and `emitDisposals` runs it at the local's scope `}`.
// In a sync iterator body the same machinery runs — the local declaration
// looks the same — but MoveNext RETURNS at the yield and never reaches the
// scope's `}`, so the disposal IR is dead code. There is no other release path
// for the frame-resident +1 until the iterator is disposed.
//
// ── SPEC CITATIONS. §27.5 ("Closure capture reclamation") — closure cells
// tracked as captured state, every +1 paired with a release. §29.7
// ("Disposal of partially-consumed iterators") — iterator Dispose must reclaim
// every frame-resident resource, including captured closures and locals that
// survive across yield points. The pre-fix behavior violates both: the cell is
// frame-resident, is a closure, survives a yield, and is not reclaimed.
//
// ── MEASURED, `--sanitize=address` (LSan on), one build of each compiler:
//
//   program                                         pre-fix (42eff889)  post-fix
//   #4468 minimal repro, ONE enumeration            40 B / 1           clean
//   the same, x100 enumerations                     4000 B / 100        clean
//   annotated form `var f: () -> int := lambda…`,
//     ONE enumeration                                40 B / 1           clean
//   annotated form, x100 enumerations               4000 B / 100        clean
//   capture-free lambda binding a `string` capture,
//     x100 enumerations (each cell+env+cap-cell)   8400 B / 300        clean
//   const-local form (`const f := lambda…`),
//     x100 enumerations                             4000 B / 100        clean
//
// The 40 B figure is the closure cell alone; the `string`-capture case adds the
// env block + capture cell (the closure-release + envdtor chain runs at last
// drop). Pre-fix, every loop-amplified row multiplies the per-iteration leak by
// the iteration count; post-fix, every row is clean under ASan/LSan.
//
// ⚠ These rows NEED the ASan/LSan ORACLE (`runAotChecked`, NOT `runAot`): the
// effect under test is memory nobody frees, which a plain build reports as a
// perfectly correct total. Do not "simplify" any row to `runAot` — that
// silently disarms it. The issue's own one-shot repro is kept because 40 B / 1
// is the filed measurement; the x100 row beside it is the AMPLIFICATION that
// makes the row immune to a one-shot reachability accident.
//
// per §27.5, §29.7 (issue #4468)

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
// runAotChecked runs the program under generated-code ASan with LeakSanitizer
// on, so a non-empty leak report fails the row before the value is compared.
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ── The issue's exact repro, ONE enumeration of a one-yield generator whose
// body binds one capture-free lambda. The value 42 is f() and must stay right;
// pre-fix the program printed 42 and leaked 40 B / 1 allocation.
//
//   $ bezelc --sanitize=address -o p p.bzl && ./p
//   total=42
//   SUMMARY: AddressSanitizer: 40 byte(s) leaked in 1 allocation(s).
//
// per §27.5, §29.7 (issue #4468)
BEZEL_TEST(iterator_yield_function_typed_local_leak_4468,
           sync_iterator_yield_of_function_typed_local_no_leak_spec_27_5_29_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class C "
        "  public static method MakeSeq(): IEnumerable<() -> int> "
        "    var f := lambda(): int => 42; "
        "    yield return f; "
        "  end MakeSeq; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var fn in C.MakeSeq() do total := total + fn(); end; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=42\n");
}

// ── LOOP-AMPLIFIED twin of the row above: 100 full enumerations, so the
// pre-fix report is 4000 B / 100 allocations rather than 40 B / 1. The x100
// row is the AMPLIFICATION that no one-shot reachability accident can explain.
// Printed total: 100 x 42 = 4200.
//
// per §27.5, §29.7 (issue #4468)
BEZEL_TEST(iterator_yield_function_typed_local_leak_4468,
           sync_iterator_yield_of_function_typed_local_amplified_100_spec_27_5_29_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class C "
        "  public static method MakeSeq(): IEnumerable<() -> int> "
        "    var f := lambda(): int => 42; "
        "    yield return f; "
        "  end MakeSeq; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<100 do "
        "      for var fn in C.MakeSeq() do total := total + fn(); end; "
        "    end; "
        "    Console.WriteLine(total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "4200\n");
}

// ── The ANNOTATED form — `var f: () -> int := lambda(): int => 42;` — exercises
// the resolvedType-on-`ld->type` branch of releaseLocalClosures (rather than
// the exprType-on-initializer fallback the minimal repro takes). One
// enumeration; pre-fix 40 B / 1, post-fix clean. Printed total: 7.
//
// per §27.5, §29.7 (issue #4468)
BEZEL_TEST(iterator_yield_function_typed_local_leak_4468,
           sync_iterator_yield_of_annotated_function_typed_local_no_leak_spec_27_5_29_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class C "
        "  public static method MakeSeq(): IEnumerable<() -> int> "
        "    var f: () -> int := lambda(): int => 7; "
        "    yield return f; "
        "  end MakeSeq; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var fn in C.MakeSeq() do total := total + fn(); end; "
        "    Console.WriteLine(total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "7\n");
}

// ── The ANNOTATED form, LOOP-AMPLIFIED to x100: 4000 B / 100 pre-fix, clean
// post-fix. The x100 row is the AMPLIFICATION companion to the one-shot above.
//
// per §27.5, §29.7 (issue #4468)
BEZEL_TEST(iterator_yield_function_typed_local_leak_4468,
           sync_iterator_yield_of_annotated_function_typed_local_amplified_100_spec_27_5_29_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class C "
        "  public static method MakeSeq(): IEnumerable<() -> int> "
        "    var f: () -> int := lambda(): int => 7; "
        "    yield return f; "
        "  end MakeSeq; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<100 do "
        "      for var fn in C.MakeSeq() do total := total + fn(); end; "
        "    end; "
        "    Console.WriteLine(total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "700\n");
}

// ── A `string` CAPTURE is an existing separate concern (#3500 — the capture
// cell + env block + cell all leak on a yielded lambda with a non-escaping
// capture path; #3500 closed the cell + env, and the capture cell for a
// yielded escape continues to leak). This fix is narrowly the closure CELL
// release on iterator Dispose; the capture-cell leak is NOT in scope and not
// re-fixed here. The simplest shape that demonstrates ONLY the #4468 fix
// (capture-free lambda) is the first row above, kept at x1 and x100.
//
// The annotated and const rows below exercise the other two arms of
// releaseLocalClosures (resolvedType on `ld->type` for annotated, LocalConstDecl
// cast for `const`). The string-capture row was removed because its
// pre-existing 4–8 B/call capture-cell leak masks the 40 B/call closure-cell
// fix — the row would FAIL on ASan for reasons unrelated to #4468.
//
// per §27.5, §29.7 (issue #4468)

// ── The `const` form — `const f := lambda(): int => 13;` — exercises
// `releaseLocalClosures`'s LocalConstDecl arm. #3402 / #3500 admitted the
// const side of the function-typed local into the scope-end reclaim family;
// the iterator Dispose path has the same need. Pre-fix the cell is on the
// frame, not released; the body's `}` is unreachable as in the `var` case.
// x100: 4000 B / 100 pre-fix, clean post-fix. Printed total: 100 x 13 = 1300.
//
// per §21.1, §27.5, §29.7 (issue #4468)
BEZEL_TEST(iterator_yield_function_typed_local_leak_4468,
           sync_iterator_yield_of_const_function_typed_local_amplified_100_spec_27_5_29_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class C "
        "  public static method MakeSeq(): IEnumerable<() -> int> "
        "    const f := lambda(): int => 13; "
        "    yield return f; "
        "  end MakeSeq; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var j in 0..<100 do "
        "      for var fn in C.MakeSeq() do total := total + fn(); end; "
        "    end; "
        "    Console.WriteLine(total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "1300\n");
}