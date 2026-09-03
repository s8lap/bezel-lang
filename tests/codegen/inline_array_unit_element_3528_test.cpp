// fix #3528 — an inline array of the UNIT type (`[3]()`) used as a LOCAL or a
// PARAMETER killed `bezelc` with SIGILL and NO DIAGNOSTIC OF ANY SEVERITY
// (spec §4.7 *The unit type*, §15.1 *Inline arrays*, §3.1/§3.2 *Type kinds* /
// *Reference vs. value types* — v0.286).
//
// MEASURED on this branch pre-fix (HEAD at the time of filing):
//
//     var a: [3]() := [(), (), ()];      -->  bezelc rc=132 (SIGILL)
//     method Take(a: [2]()): int => 1;   -->  bezelc rc=132 (SIGILL), on the
//                                             DECLARATION alone (Take is never
//                                             called)
//
// Under gdb both traps land in the SAME frame:
//
//     #0  llvm::DataLayout::getPrefTypeAlign(llvm::Type*) const
//     #1  llvm::IRBuilderBase::CreateAlloca                (IRBuilder.h:1845)
//     #2  bezel::codegen::Emitter::entryAlloca              (impl.hpp)
//     #3  Emitter::lowerLocalDecl / Emitter::bindParameters (LlvmBackend_Setup.cpp)
//
// ── ROOT CAUSE — the ELEMENT position of the #3453/#3527 substitution family
// ───────────────────────────────────────────────────────────────────────────
//
// §4.7 gives `()` exactly one value; `lowerTypeRef` answers LLVM `void` for
// exactly that one type, and every STORAGE position is supposed to substitute
// the one inert byte (`unitStorageTy`, minted by fix #3527) instead —
// `entryAlloca`, `lowerParamType`/`lowerParamTypeRef`, `storageType`'s three
// member arms, and the state-machine frame planner (#3453) all do. Every one
// of those tests the TOP-LEVEL lowered type. `Emitter::lowerTypeRef`'s
// `Kind::InlineArray` arm (LlvmBackend_Layout.cpp) built
// `llvm::ArrayType::get(elem, N)` from the RAW element lowering — the unit
// substitution is one level DOWN from every existing call site, so none of
// them saw it. `ArrayType::get` does not itself validate a `void` element in a
// release (assertions-off) LLVM, so the trap surfaced one step later, at the
// array's OWN alloca (`entryAlloca` substitutes correctly at the TOP level —
// `[3 x void]` is not itself void, so that guard never fires).
//
// ── WHY `[N]()` IS AN ADMITTED SHAPE, NOT A REJECTION ─────────────────────────
//
// §3.2 (*Reference vs. value types*) states the classification is "total and
// disjoint" and lists the unit type among the value types explicitly. §15.1's
// element-type rule reads over that same total classification — "a value
// type", "a class type", "a runtime-managed reference type" — with NO
// unit-type carve-out, unlike the explicit `[N][M]T` multi-dimensional
// exclusion stated two sentences later in the same section. §17.11 (*The
// `default` operator*) lists "The unit type: `()`" among the defaultable
// per-kind values, so `[N]()` is defaultable too (its own "inline arrays whose
// element type is defaultable" bullet), consistent with T3 below declaring one
// with no initializer. The fix therefore ADMITS the shape (lowers it) rather
// than rejecting it at sema.
//
// THE FIX asks the existing #3527 authority rather than adding a NINTH
// spelling of the same decision: `lowerTypeRef`'s `Kind::InlineArray` arm now
// wraps the element in `unitStorageTy`, exactly as every sibling storage
// position already does. That alone is enough for the PARAMETER repro (T2):
// `lowerParamType`/`lowerParamTypeRef` call `lowerTypeRef` internally, so the
// signature's `[2 x i8]` is already correct once the element substitution is
// in place — MEASURED: after the Layout.cpp-only change, T2 compiles and runs
// clean while T1 still failed (see below).
//
// The LOCAL repro (T1) needed one more guard: `lowerLocalDecl`'s
// `CollectionLiteralExpr` branch (LlvmBackend_Setup.cpp) stores each literal
// element via `lowerExpr`, and `lowerExpr` normalises every UNIT-typed
// expression to `nullptr` by the SAME established convention fix #3421's
// guard documents ("a unit-typed expression has no lowered value; every
// producer must report it as one"). Before this fix that null was
// indistinguishable from a genuine lowering failure and hit the pre-existing
// `#1386` internal-error guard — turning the crash into a bogus ICE
// diagnostic on a LEGAL program (`[(), (), ()]`), which is not an acceptable
// substitute for "compiles and runs correctly" (CLAUDE.md / LANE_RULES §6c: a
// loud-but-WRONG diagnostic on a legal program is not the goal either). The
// added guard: when the source element's own sema type is `Kind::Unit`, store
// the placeholder slot's zero value instead of reporting the ICE — the same
// "one inert byte, never meaningfully read" reasoning #3453/#3527 apply to
// every other unit storage position.
//
// ── WHY THESE ROWS ────────────────────────────────────────────────────────────
//
// T1/T2 are the issue's own minimal repros — pre-fix both fail at `r.ran`
// (SIGILL, no diagnostic of any kind). T3 is a LAYOUT oracle: a legal-but-
// wrong fix (a zero-width or over-wide element slot) would silently corrupt
// the locals declared immediately before/after the array on the stack, which
// this row's neighbour reads would catch. T4 exercises the no-initializer /
// `default()` path (§17.11), a distinct lowering branch from the literal-store
// loop T1/T3 exercise. T5 is the PASS-BY-VALUE / iteration / equality surface
// the brief calls out explicitly — `Length`, `for..in`, indexing yielding
// `()`, structural `=`. T6 is a BOTH-SIDES CONTROL: the same shapes with an
// ordinary `int` element, so a harness break or an unrelated regression in
// this file would not pass silently, and so this fix is pinned to change
// nothing about an ordinary (non-unit) inline array.
//
//   FAIL-WITHOUT (src/ restored to the pre-fix parent, this test file kept):
//     T1  0 passed, 1 failed  (r.ran == false, SIGILL)
//     T2  0 passed, 1 failed  (r.ran == false, SIGILL)
//     T3  0 passed, 1 failed  (r.ran == false, SIGILL)
//     T4  0 passed, 1 failed  (r.ran == false, SIGILL)
//     T5  0 passed, 1 failed  (r.ran == false, SIGILL)
//     T6  1 passed, 0 failed  (ordinary int array — unaffected either way)
//   PASS-WITH: 6 passed, 0 failed (bezel_tests_codegen).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// ── T1 — THE ISSUE'S MINIMAL LOCAL REPRO. Pre-fix rc=132 with no diagnostic;
// post-fix the program runs and prints its marker.
BEZEL_TEST(codegen_inline_array_unit_element_3528,
           unit_element_inline_array_local_literal_spec_4_7_15_1) {
    auto r = runAot(
        "module M3528A; "
        "class C "
        "  public static method Main(): int "
        "    var a: [3]() := [(), (), ()]; "
        "    Console.WriteLine(\"ok\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}

// ── T2 — THE ISSUE'S MINIMAL PARAMETER REPRO. `Take` is DECLARED and never
// called — the crash (pre-fix) fires purely at signature/parameter-binding
// lowering, not at a call site.
BEZEL_TEST(codegen_inline_array_unit_element_3528,
           unit_element_inline_array_parameter_declaration_spec_4_7_15_1) {
    auto r = runAot(
        "module M3528B; "
        "class C "
        "  public static method Take(a: [2]()): int => 1; "
        "  public static method Main(): int "
        "    Console.WriteLine(\"ok\"); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}

// ── T3 — LAYOUT ORACLE. Two `int` locals bracket the unit-element array on
// the stack; a wrong-width element slot (zero bytes, or widened past the
// one-byte placeholder) would corrupt one or both neighbours. `Length` and
// `a[1].ToString()` (§15.1.2 indexer, §8.5 unit rendering) are read too, so a
// fix that merely stops the trap without laying the array out correctly still
// fails this row.
BEZEL_TEST(codegen_inline_array_unit_element_3528,
           unit_element_array_does_not_disturb_stack_neighbours_spec_15_1_2_8_5) {
    auto r = runAot(
        "module M3528C; "
        "class C "
        "  public static method Main(): int "
        "    var before: int := 11; "
        "    var a: [3]() := [(), (), ()]; "
        "    var after: int := 22; "
        "    Console.WriteLine(before); "
        "    Console.WriteLine(a.Length); "
        "    Console.WriteLine(a[1].ToString()); "
        "    Console.WriteLine(after); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("11\n3\n()\n22\n"));
}

// ── T4 — the NO-INITIALIZER path (§15.1 "filled with that element default per
// slot") and the explicit `default(T)` operator (§17.11: "The unit type:
// `()`", so `default([N]())` is itself defaultable). Neither reaches the
// CollectionLiteralExpr store loop T1/T3 exercise — the array is
// zero-initialized as a whole (`ConstantAggregateZero`) — so this pins a
// DIFFERENT lowering branch reached the same array-type substitution.
BEZEL_TEST(codegen_inline_array_unit_element_3528,
           unit_element_array_default_initialization_spec_17_11_15_1) {
    auto r = runAot(
        "module M3528D; "
        "class C "
        "  public static method Main(): int "
        "    var a: [3](); "
        "    Console.WriteLine(a.Length); "
        "    Console.WriteLine(a[0].ToString()); "
        "    var d: [4]() := default([4]()); "
        "    Console.WriteLine(d.Length); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n()\n4\n"));
}

// ── T5 — PASS-BY-VALUE, ITERATION and STRUCTURAL EQUALITY. `Sum` receives the
// array BY VALUE (a fresh parameter slot, §15.1's value-type copy semantics)
// and iterates it with `for..in` (§15.1.2: "The compiler emits a direct
// indexed loop"); a copy-assigned second local reads back correctly, and `=`
// exercises §15.1.2's "pairwise equal under the element type's equality" —
// trivially true for unit, but only if the slot layout is sound.
BEZEL_TEST(codegen_inline_array_unit_element_3528,
           unit_element_array_pass_by_value_iteration_equality_spec_15_1_15_1_2) {
    auto r = runAot(
        "module M3528E; "
        "class C "
        "  public static method Sum(a: [3]()): int "
        "    var n: int := 0; "
        "    for var x in a do "
        "      n := n + 1; "
        "    end; "
        "    return n; "
        "  end Sum; "
        "  public static method Main(): int "
        "    var a: [3]() := [(), (), ()]; "
        "    Console.WriteLine(Sum(a)); "
        "    var b: [3]() := a; "
        "    Console.WriteLine(b[0].ToString()); "
        "    Console.WriteLine(a[0] = a[1]); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n()\nTrue\n"));
}

// ── T6 — BOTH-SIDES CONTROL. T1-T5 all fail at `r.ran` pre-fix (a SIGILL
// crashes the whole test process — LANE_RULES §3), so nothing in this file
// would notice a harness break independent of this fix. This row is T3/T5
// with the unit element replaced by `int`: the same bracketing locals, the
// same `Length`/indexer/iteration/equality surface, no unit type anywhere —
// so it passes on BOTH sides of the fix and pins that this commit does not
// change the layout of an ordinary inline array.
BEZEL_TEST(codegen_inline_array_unit_element_3528,
           control_int_element_array_unaffected_spec_15_1) {
    auto r = runAot(
        "module M3528F; "
        "class C "
        "  public static method Sum(a: [3]int): int "
        "    var n: int := 0; "
        "    for var x in a do "
        "      n := n + x; "
        "    end; "
        "    return n; "
        "  end Sum; "
        "  public static method Main(): int "
        "    var before: int := 11; "
        "    var a: [3]int := [1, 2, 3]; "
        "    var after: int := 22; "
        "    Console.WriteLine(before); "
        "    Console.WriteLine(a.Length); "
        "    Console.WriteLine(a[1].ToString()); "
        "    Console.WriteLine(after); "
        "    Console.WriteLine(Sum(a)); "
        "    var b: [3]int := a; "
        "    Console.WriteLine(b[0].ToString()); "
        "    Console.WriteLine(a[0] = a[1]); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    std::string("11\n3\n2\n22\n6\n1\nFalse\n"));
}
