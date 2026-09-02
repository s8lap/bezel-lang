// Codegen — issue #3591 (LocalDecl-only walker family), four sites touched
// in `src/codegen/LlvmBackend_Setup.cpp` in the same pass. Each was a
// `dynamic_cast<const ast::LocalDecl*>(...)` with no sibling arm for the
// §21.1 runtime `const` local (`ast::LocalConstDecl` — a DIFFERENT AST node,
// initializer field `value` rather than `initializer`, no shared base beyond
// `ast::Stmt`). Per this run's warning that a "never reaches codegen" claim
// must be TESTED, not assumed, every claim below was checked against a real
// compile — which is why Site A is reported as REACHABLE-BUT-UNPROVEN rather
// than fixed-with-a-leak-repro: three sibling lanes this cycle had an
// "unreachable" claim disproven by writing the `const` twin, and the same
// discipline applied here in the OTHER direction — a claimed leak that did
// not reproduce.
//
// SITE A — `Emitter::lowerLocalDecl`'s `borrowingArrayFreeEligible_` consumer
// (§28.9 / §28.11, stdlib §5/§9.6): the per-element string-release-thunk
// derivation used a bare `dynamic_cast<const ast::LocalDecl*>(declNode)`,
// always null for a `const`, widened to the shared `localDeclInitView`
// (matching the #2802/#3417 precedent this file uses everywhere else in the
// same family). REACHABILITY, NOT LEAK, IS WHAT WAS PROVEN: the two shapes
// that would exercise the missing element-type derivation with a NON-null
// `el` are BOTH sema-rejected for `const` (measured directly) —
// `const p := "…".Split(",");` ("a collection materializer returns a fresh
// owned collection ... bind it with using var") and a range slice
// `const s := arr[r];` ("a range slice ... produces a fresh owned
// Array<T> ..."). The one admitted shape, a `const` ARRAY LITERAL of
// heap-produced strings (`const a: Array<string> := [1.ToString(), …];`),
// DOES reach the consumer (`borrowingArrayFreeEligible_` admits it per
// `isBorrowingArrayLiteralLocal`'s element-policy gate) but showed NO
// leak-under-ASan differential in EITHER direction — checked by temporarily
// reverting just this hunk, rebuilding `bezelc`, and re-running the same
// 500-iteration probe (clean before AND after) — which points at collection
// literals not retaining a separate co-owned +1 the way `.ToArray()` does,
// so there is nothing this particular element-disposer omission drops.
// Row A below pins the one admitted shape as a plain correctness/cleanliness
// regression guard, not a FAIL-WITHOUT/PASS-WITH claim; the code change
// itself is kept (it is the established, harmless, correct-by-construction
// pattern every sibling site in this file uses) but is NOT reported as a
// proven bug fix.
//
// SITE D — `Emitter::isObjectStringSource` (§28.9): the REASSIGNMENT-time
// twin of Site A's source question. On `o := s;` reassigning an
// Object-typed local already tracked in `objectStringBorrowAll_`, this
// decides whether the NEW source `s` is a bare string identifier, to set
// the release-tracking state bit. A bare `dynamic_cast<const
// ast::LocalDecl*>(loc->decl)` on the source's OWN declaration was always
// null for a `const` source, so the bit was wrongly cleared even though the
// slot now holds a genuine string — disarming the scope-end
// `bz_string_release`. Widened to the shared `localDeclInitView`, mirroring
// Site A and the #3417 precedent. FAIL-WITHOUT/PASS-WITH, mechanically
// verified (reverted just this hunk, rebuilt `bezelc`, restored): pre-fix
// leaks 17000 B / 1000 allocations for a 500-iteration loop of `var o:
// Object := a; o := b;` with `a` a plain string local and `b` a `const`
// string local; post-fix clean, correct. The `var`-bound control is clean
// on both sides.
//
// SITE B — the `LambdaFinder::walkStmt` capture-symbol walk inside
// `Emitter::collectCellFreeEligible` (§27.5 Closures: "Captured variables
// are heap-allocated in a closure object"). This walk finds every OUTER
// symbol a lambda body references, to classify the symbol's ALREADY-boxed
// capture cell as `cellFreeEligible_` (safe to free at scope end). It had no
// `LocalConstDecl` arm, so an outer local referenced ONLY through a
// `const`'s initializer inside the lambda body (`const doubled := count *
// 2;`) was invisible to the walk — `count`'s cell (unconditionally boxed by
// `boxLocalSlot` for every captured symbol, `LlvmBackend_Exprs2.cpp`) never
// entered `cellFreeEligible_` and leaked. Measured on the pre-fix compiler:
// 2000 B / 500 allocations under ASan/LSan for a 500-iteration loop calling
// a method whose only lambda's only reference to a captured `var` was via
// such a `const`.
//
// SITE C — the `findLamsS` walk inside the same function's #1135
// nested-lambda-escape widening (§27.5, #1135: a grandparent local shared
// between an outer non-escaping lambda and an inner lambda that escapes the
// outer one must not have its cell freed at the OUTER lambda's scope end).
// `findLamsS` finds nested `LambdaExpr` literals inside an outer lambda's
// body so the grandparent local's cell is excluded from
// `cellFreeEligible_`; it had no `LocalConstDecl` arm, widened here to match
// its `LocalDecl` sibling (same rationale/shape as Site B). REACHABILITY,
// NOT A const-VS-var DIFFERENTIAL, IS WHAT WAS PROVEN: `var outer :=
// lambda() => do const inner := lambda() => x; return inner; end; return
// outer();` leaks 52 B / 3 allocations under ASan/LSan (500-iteration total:
// 26000 B / 1500 allocations) — but so does the BYTE-IDENTICAL `var inner`
// twin, and so does dropping the intermediate binding entirely (`return
// lambda() => x;` directly inside the `do...end` block), all three
// measured at the IDENTICAL 52 B/call. This is a pre-existing, more general
// leak in a BLOCK-BODIED outer lambda returning an escaping nested lambda —
// unrelated to the LocalDecl/LocalConstDecl split this file's family
// targets (the passing `codegen_closures_27_5_1135` tests in
// closure_capture_test.cpp all use the EXPRESSION-bodied spelling,
// `lambda() => lambda() => x`, never `do...end`). Filed as #3704
// (a NEW issue, not fixed here — out of this file's LocalDecl/LocalConstDecl
// scope and requiring separate root-causing). The `findLamsS` widening is
// kept (matches Site B's proven-necessary shape and cannot make this
// pre-existing leak worse) but is not claimed to close an observable bug.
//
// Every row uses `runAotChecked` (generated-code ASan with LeakSanitizer), so
// `r.ran` is itself the leak assertion (§R12-G) — this file makes no
// crash-signature claim, only a value-and-cleanliness one. Sites B and D's
// leak-prone shapes FAIL (LSan flags it, `r.ran == false`) on the pre-fix
// compiler and PASS (clean, correct value) with the fix, each `const` row
// paired with a byte-identical `var` control clean on BOTH sides — these
// are the two rows in this file with a genuine, proven FAIL-WITHOUT/
// PASS-WITH. Sites A and C are plain clean-and-correct regression guards
// (see the header notes above): both hold on the pre-fix AND post-fix
// compiler, so neither proves its widening closed an observable bug — that
// is reported honestly rather than dressed up as a proven fix.
//
// VALUE OBSERVATION: the 500-iteration totals below (3500 / 1500, and Site
// A's element count) all exceed 255, so this file reports through STDOUT
// rather than the AOT process exit code, which truncates to 8 bits
// (LANE_RULES's runet-vs-exit note) — `Console.WriteLine(total.ToString())`,
// compared against `stdoutText` (which, unlike `runAotOut`, keeps its
// trailing newline).
// per §21.1, §27.5, §28.9, §28.11, stdlib §5, stdlib §9.6 (issue #3591)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// r.ran ? r.stdoutText : "<did-not-run>" — a leak (pre-fix) makes
// runAotChecked report ran=false; the sentinel keeps a single EQ assertion
// for both memory cleanliness and the computed value (mirrors
// local_const_subscription_dispose_3591_test.cpp's `run` helper).
std::string runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

}  // namespace

// ── SITE A: borrowingArrayFreeEligible_'s element-disposer consumer ───────

// Row A (coverage, NOT a leak-differential claim — see the header note
// above): the one shape that reaches this consumer with a `const` `declNode`
// — a `const` ARRAY LITERAL of heap-produced (non-literal) strings, never
// escaping, looped 500x. Pins the value AND ASan/LSan cleanliness on the
// current (fixed) compiler; both held on the pre-fix compiler too, so this
// row is a regression guard for the widened accessor, not evidence it closed
// an observable bug.
BEZEL_TEST(localdecl_bare_walkers_setup_3591,
           const_array_literal_heap_strings_clean_28_9_28_11) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class C "
        "  public static method Run(): int "
        "    const parts: Array<string> := "
        "        [1.ToString(), 2.ToString(), 3.ToString()]; "
        "    return parts.Count; "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..500 do "
        "      total := total + C.Run(); "
        "    end; "
        "    Console.WriteLine(total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "1500\n");
}

// Row A control: byte-identical except `var` in place of `const`. Clean on
// both the pre-fix and post-fix compiler — the `var` cast always succeeded.
BEZEL_TEST(localdecl_bare_walkers_setup_3591,
           var_array_literal_heap_strings_clean_28_9_28_11_control) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class C "
        "  public static method Run(): int "
        "    var parts: Array<string> := "
        "        [1.ToString(), 2.ToString(), 3.ToString()]; "
        "    return parts.Count; "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..500 do "
        "      total := total + C.Run(); "
        "    end; "
        "    Console.WriteLine(total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "1500\n");
}

// ── SITE D: isObjectStringSource's REASSIGNMENT-time source check ────────

// Row D1 (repro): reassigning an Object-typed local that is already
// tracked in objectStringBorrowAll_ (bound from a bare string identifier)
// FROM a `const` string source. isObjectStringSource decides the new
// state-bit value (§28.9's "is the current slot value a string" bit) for
// the reassignment; a bare `dynamic_cast<const ast::LocalDecl*>(loc->decl)`
// on the SOURCE identifier's declaration was always null for a `const`
// source, so the state bit was wrongly cleared to "not a string" even
// though the slot genuinely now holds one -- disarming the scope-end
// bz_string_release for that value. Looped 500x. FAIL-WITHOUT/PASS-WITH,
// mechanically verified (reverted just this hunk, rebuilt bezelc, restored):
// pre-fix leaks 17000 B / 1000 allocations under ASan/LSan; post-fix clean.
BEZEL_TEST(localdecl_bare_walkers_setup_3591,
           const_source_reassignment_state_bit_armed_28_9) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class C "
        "  public static method Run() "
        "    var a := \"initial\"; "
        "    const b := (2).ToString(); "
        "    var o: Object := a; "
        "    o := b; "
        "  end Run; "
        "  public static method Main(): int "
        "    for var i in 1..500 do "
        "      C.Run(); "
        "    end; "
        "    Console.WriteLine(\"0\"); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "0\n");
}

// Row D2 (control): byte-identical except the source is `var`-bound. Clean
// before and after this fix -- the `var` cast always succeeded.
BEZEL_TEST(localdecl_bare_walkers_setup_3591,
           var_source_reassignment_state_bit_armed_28_9_control) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class C "
        "  public static method Run() "
        "    var a := \"initial\"; "
        "    var b := (2).ToString(); "
        "    var o: Object := a; "
        "    o := b; "
        "  end Run; "
        "  public static method Main(): int "
        "    for var i in 1..500 do "
        "      C.Run(); "
        "    end; "
        "    Console.WriteLine(\"0\"); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "0\n");
}

// ── SITE B: LambdaFinder::walkStmt's capture-symbol walk ──────────────────

// Row B1 (repro): a captured `var count` referenced ONLY through a nested
// `const`'s initializer inside a non-escaping lambda (`f(); ` called once,
// never stored/returned/passed on), looped 500x. `count` itself is never
// assigned or read anywhere else in the lambda body, so only the
// LocalConstDecl arm can see it. Expected: `count` unchanged (7) each call,
// total 500*7 = 3500.
BEZEL_TEST(localdecl_bare_walkers_setup_3591,
           const_only_capture_reference_cell_freed_27_5) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class C "
        "  public static method Run(): int "
        "    var count := 7; "
        "    var f := lambda() => do "
        "      const doubled := count * 2; "
        "      _ := doubled; "
        "    end; "
        "    f(); "
        "    return count; "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..500 do "
        "      total := total + C.Run(); "
        "    end; "
        "    Console.WriteLine(total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "3500\n");
}

// Row B2 (control): byte-identical except the nested binding is `var`. Clean
// before and after this fix.
BEZEL_TEST(localdecl_bare_walkers_setup_3591,
           var_only_capture_reference_cell_freed_27_5_control) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class C "
        "  public static method Run(): int "
        "    var count := 7; "
        "    var f := lambda() => do "
        "      var doubled := count * 2; "
        "      _ := doubled; "
        "    end; "
        "    f(); "
        "    return count; "
        "  end Run; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..500 do "
        "      total := total + C.Run(); "
        "    end; "
        "    Console.WriteLine(total.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "3500\n");
}

// ── SITE C: findLamsS's #1135 nested-lambda-escape widening ───────────────
//
// NOT tested with runAotChecked: this exact construct leaks the grandparent
// cell regardless of the `const`/`var` choice (issue #3704, header note
// above) — an ASan/LSan row here would fail on BOTH sides and would prove
// nothing about this file's LocalDecl/LocalConstDecl family. Value-only
// (plain runAot) rows below pin that the WIDENING itself is correctness-safe
// (right answer, both bindings) without making a cleanliness claim #3704
// already tracks separately.

namespace {
int runValue(const std::string& prog) {
    auto r = bezel::test::runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// Row C1: `const`-bound nested escaping lambda — correct value (3) despite
// #3704's leak. Mirrors closure_capture_test.cpp's
// `codegen_closures_27_5_1135` `var`-nested tests, inner binding changed to
// `const`, block-bodied per Site C's construct.
BEZEL_TEST(localdecl_bare_walkers_setup_3591,
           const_nested_lambda_escapes_grandparent_value_27_5_1135) {
    BEZEL_EXPECT_EQ(runValue(
        "module M; "
        "class C "
        "  public static method Make(): () -> int "
        "    var x := 3; "
        "    var outer := lambda() => do "
        "      const inner := lambda() => x; "
        "      return inner; "
        "    end; "
        "    return outer(); "
        "  end Make; "
        "  public static method Main(): int "
        "    var f := C.Make(); "
        "    return f(); "
        "  end Main; "
        "end C;"),
        3);
}

// Row C2 (control): byte-identical except the inner lambda is `var`-bound.
BEZEL_TEST(localdecl_bare_walkers_setup_3591,
           var_nested_lambda_escapes_grandparent_value_27_5_1135_control) {
    BEZEL_EXPECT_EQ(runValue(
        "module M; "
        "class C "
        "  public static method Make(): () -> int "
        "    var x := 3; "
        "    var outer := lambda() => do "
        "      var inner := lambda() => x; "
        "      return inner; "
        "    end; "
        "    return outer(); "
        "  end Make; "
        "  public static method Main(): int "
        "    var f := C.Make(); "
        "    return f(); "
        "  end Main; "
        "end C;"),
        3);
}
