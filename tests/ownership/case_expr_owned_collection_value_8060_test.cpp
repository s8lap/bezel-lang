// Regression tests for #8060 -- a `case` EXPRESSION (and its `if`-expression
// twin) whose arms produce an owned BUILTIN COLLECTION leaked that value when
// it was bound: `var u := (case 1 of _ => new OwningArray<Cell>());` leaked
// the `bz_array_t` header and its backing buffer (208 bytes in 2 allocations
// per evaluation).
//
// THE DEFECT (measured). The ownership pass classifies an if/case expression
// through the shared arm-meet predicate `common::conditionalCaseArmMeetIsOwned`
// (src/common/OwnershipFacts.cpp). Its per-arm test asked only
// `isFreshOwnedClassProducer`, which ends in "the result type's declaration is
// a CLASS" -- and every builtin collection is declaration-less. So two
// `new OwningArray<Cell>()` arms met to BORROWED: `producerState` called the
// construct a borrow, the binding carried no obligation, no scope-end disposal
// was registered, and the collection leaked. The same misclassification made
// a NESTED owned-collection meet a borrow ARM, so a nested `case` beside a
// fresh `new` arm was wrongly REJECTED as a mixed owned/borrowed meet.
//
// THE SPEC (language spec v0.355).
//
//   §28.3 producers table, the if/case row: "`(if c then v1 else v2)`; the
//   result of an expression-position `case` -- The **meet** of the arms'
//   ownership states, identically to the `a ?? b` row above (K7 at
//   expression joins; for `case` the meet ranges over every arm's result
//   expression)".
//
//   §28.3.1 K7, clause 1: "owned ⊓ owned = owned, obligation carried forward
//   (for expression arms: exactly one arm's value exists at runtime, so
//   exactly one obligation)".
//
//   §19.2 "Arms": "In expression position, the ownership of the `case`
//   result follows the arm-meet rule of §28.3's producers table -- identical
//   to `??` and the `if` expression (§20)".
//
// A `new` of a builtin collection and an `owned`-returning call are owned
// producers (the same table's `new` and owned-return rows), so the meet of
// such arms is OWNED and the binding that receives it owns -- and disposes --
// it exactly once.
//
// THE ORACLE. Every leak row runs 100 iterations (a one-shot leak repro is a
// vacuous oracle) and measures a COUNT: the allocations LeakSanitizer reports
// as leaked, parsed from LSan's "SUMMARY: AddressSanitizer: <bytes> byte(s)
// leaked in <allocations> allocation(s)." line by the harness's own parser.
// The expected count is ZERO. On the pre-fix compiler the rows read 200
// allocations / 20800 bytes (the header and backing buffer of each array),
// or the counts quoted on the row. Every row also asserts the exact stdout
// and that no ASan report of any kind is present, so a double free (the
// direction a wrong fix would fail in) fails the row too.
//
// The CONTROL rows are the shapes that must NOT change: a class-typed arm
// (already disposed through the class predicate), a value-typed `string` arm,
// a borrow meet over two named owned locals (each disposed by its own scope;
// a wrong "owned" classification would double-free), the `owned`-argument and
// `return` positions (already transferring), and the K7.3 mixed-meet
// rejection (owned `new` beside an unmarked named-local read).
//
// Spec: §28.3 producers table (if/case arm-meet row), §28.3.1 K7.1 / K7.3,
// §19.2 "Arms", §20 "Ownership" (v0.355); stdlib §4.4-§4.11 (the builtin
// collections).

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// The program under test: `MkA()` is an `owned`-returning OwningArray
// producer, `Take` an `owned`-parameter consumer, `R` an `owned` return of a
// case expression; `body` runs inside a 100-iteration loop over `i` and adds
// into `s`, which Main prints.
std::string prog(const std::string& body) {
    return
        "module M; import System; import System.Collections.Generic; "
        "class Cell "
        "  public v: int := 0; "
        "end Cell; "
        "class D "
        "  public n: int := 1; "
        "end D; "
        "class C "
        "  public static method MkA(): owned OwningArray<Cell> "
        "    var a := new OwningArray<Cell>(); "
        "    a.Add(new Cell()); "
        "    return a; "
        "  end MkA; "
        "  public static method Take(owned a: OwningArray<Cell>): int "
        "    return a.Count; "
        "  end Take; "
        "  public static method R(i: int): owned OwningArray<Cell> "
        "    return (case i mod 2 of 0 => new OwningArray<Cell>(), _ => MkA()); "
        "  end R; "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var i in 0..<100 do "
        + body +
        "    end for; "
        "    Console.WriteLine(s); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

void expectNoLeak(const JitResult& r, const char* out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary = bezel::test::aot_detail::parseLeakSummary(
        text, leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocations
                  << " allocation(s) / " << leakedByteCount
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
}

}  // namespace

// =========================================================================
// The LEAK rows -- an owned-collection arm meet bound to a name.
// =========================================================================

// THE ISSUE'S OWN REPRO, looped: a `case` expression whose only arm is a
// fresh `new OwningArray<Cell>()`, bound by `var`. Pre-fix: 200 allocations /
// 20800 bytes leaked.
BEZEL_TEST(case_expr_owned_collection_value_8060,
           var_binding_of_new_owning_array_arm_spec_28_3_arm_meet) {
    JitResult r = runAotChecked(prog(
        "      var u := (case i of _ => new OwningArray<Cell>()); "
        "      s := s + u.Count; "));
    expectNoLeak(r, "0\n");
}

// The arm values are `owned`-returning CALLS (`MkA()`), each carrying one
// owned element -- the element cascade must run too. Pre-fix: 300
// allocations / 22400 bytes (each array's header, buffer and element).
BEZEL_TEST(case_expr_owned_collection_value_8060,
           var_binding_of_owned_call_arms_spec_28_3_arm_meet) {
    JitResult r = runAotChecked(prog(
        "      var u := (case i mod 3 of 0 => MkA(), _ => MkA()); "
        "      s := s + u.Count; "));
    expectNoLeak(r, "100\n");
}

// A `new` arm beside an `owned`-call arm: owned ⊓ owned (K7.1). Pre-fix: 250
// allocations / 21600 bytes.
BEZEL_TEST(case_expr_owned_collection_value_8060,
           var_binding_of_new_and_owned_call_arms_spec_28_3_1_k7_1) {
    JitResult r = runAotChecked(prog(
        "      var u := (case i mod 2 of 0 => new OwningArray<Cell>(), _ => MkA()); "
        "      s := s + u.Count; "));
    expectNoLeak(r, "50\n");
}

// Elements added through the bound name after the construct: the scope-end
// disposal must cascade to them. Pre-fix: 300 allocations / 22400 bytes.
BEZEL_TEST(case_expr_owned_collection_value_8060,
           var_binding_then_add_owned_element_spec_28_3_arm_meet) {
    JitResult r = runAotChecked(prog(
        "      var u := (case i of _ => new OwningArray<Cell>()); "
        "      u.Add(new Cell()); "
        "      s := s + u.Count; "));
    expectNoLeak(r, "100\n");
}

// A BORROWING-family collection (`Array<int>`) -- the binding still owns the
// container itself. Pre-fix: 200 allocations / 14400 bytes.
BEZEL_TEST(case_expr_owned_collection_value_8060,
           var_binding_of_new_array_arm_spec_28_3_arm_meet) {
    JitResult r = runAotChecked(prog(
        "      var u := (case i of _ => new Array<int>()); "
        "      s := s + u.Count; "));
    expectNoLeak(r, "0\n");
}

// A dictionary-backed collection, written to after binding. Pre-fix: 400
// allocations / 41800 bytes.
BEZEL_TEST(case_expr_owned_collection_value_8060,
           var_binding_of_new_dictionary_arm_spec_28_3_arm_meet) {
    JitResult r = runAotChecked(prog(
        "      var u := (case i of _ => new Dictionary<string, int>()); "
        "      u[\"a\"] := 1; "
        "      s := s + u.Count; "));
    expectNoLeak(r, "100\n");
}

// The `if`-expression twin (§20 "Ownership": the same arm-meet row). Pre-fix:
// 200 allocations / 20800 bytes.
BEZEL_TEST(case_expr_owned_collection_value_8060,
           if_expression_twin_var_binding_spec_20_ownership) {
    JitResult r = runAotChecked(prog(
        "      var u := (if i > 50 then new OwningArray<Cell>() "
        "                else new OwningArray<Cell>()); "
        "      s := s + u.Count; "));
    expectNoLeak(r, "0\n");
}

// `using var` over an owned meet: §28.10 "An expression whose arm-meet is
// owned ... is an owned producer for this section's admission" -- bound
// owned and disposed at scope end. Pre-fix: 200 allocations / 20800 bytes.
BEZEL_TEST(case_expr_owned_collection_value_8060,
           using_var_binding_spec_28_10_owned_arm_meet) {
    JitResult r = runAotChecked(prog(
        "      using var u := (case i of _ => new OwningArray<Cell>()); "
        "      s := s + u.Count; "));
    expectNoLeak(r, "0\n");
}

// Reassignment of the owned local from a second owned meet: the displaced
// value is dropped on overwrite and the last one at scope end. Pre-fix: 500
// allocations / 43200 bytes.
BEZEL_TEST(case_expr_owned_collection_value_8060,
           reassignment_from_owned_meet_drops_displaced_spec_28_3) {
    JitResult r = runAotChecked(prog(
        "      var u := (case i of _ => new OwningArray<Cell>()); "
        "      u := (case i of _ => MkA()); "
        "      s := s + u.Count; "));
    expectNoLeak(r, "100\n");
}

// The exception edge: the bound value is disposed when the block unwinds
// before scope end (every even iteration). Pre-fix: 300 allocations / 22400
// bytes, the bound array leaking on both edges.
BEZEL_TEST(case_expr_owned_collection_value_8060,
           bound_value_disposed_on_exception_edge_spec_28_3) {
    JitResult r = runAotChecked(prog(
        "      try "
        "        var u := (case i of _ => MkA()); "
        "        if i mod 2 = 0 then "
        "          throw new InvalidOperationException(\"x\"); "
        "        end if; "
        "        s := s + u.Count; "
        "      catch "
        "        InvalidOperationException => do s := s + 10; end "
        "      end try; "));
    expectNoLeak(r, "550\n");
}

// A NESTED owned-collection meet beside a fresh `new` arm: owned ⊓ owned. The
// pre-fix compiler classified the inner `case` a borrow arm and REJECTED the
// program as a mixed meet ("the arms of this if/case expression mix an owned
// value with a borrow"); it must compile, run, and leak nothing.
BEZEL_TEST(case_expr_owned_collection_value_8060,
           nested_owned_meet_arm_is_owned_not_mixed_spec_28_3_1_k7_1) {
    JitResult r = runAotChecked(prog(
        "      var u := (case i mod 2 of "
        "                  0 => (case i of _ => new OwningArray<Cell>()), "
        "                  _ => new OwningArray<Cell>()); "
        "      s := s + u.Count; "));
    BEZEL_EXPECT_TRUE(diagnosticText(r).find("mix an owned value with a borrow") ==
                      std::string::npos);
    expectNoLeak(r, "0\n");
}

// =========================================================================
// The CONTROL rows -- shapes that must not change.
// =========================================================================

// An `owned` parameter consumes the meet's value (already clean pre-fix; the
// fix must not add a second disposal at the call site).
BEZEL_TEST(case_expr_owned_collection_value_8060,
           control_owned_argument_position_spec_22_3) {
    JitResult r = runAotChecked(prog(
        "      s := s + Take((case i of _ => MkA())); "));
    expectNoLeak(r, "100\n");
}

// A `return` of the meet from an `owned`-returning method transfers it to the
// caller's binding (already clean pre-fix).
BEZEL_TEST(case_expr_owned_collection_value_8060,
           control_owned_return_position_spec_28_3) {
    JitResult r = runAotChecked(prog(
        "      var u := R(i); "
        "      s := s + u.Count; "));
    expectNoLeak(r, "50\n");
}

// A class-typed owned meet (already disposed through the class predicate).
BEZEL_TEST(case_expr_owned_collection_value_8060,
           control_class_typed_arm_spec_28_3_arm_meet) {
    JitResult r = runAotChecked(prog(
        "      var u := (case i of _ => new D()); "
        "      s := s + u.n; "));
    expectNoLeak(r, "100\n");
}

// A value-typed `string` meet carries no ownership obligation.
BEZEL_TEST(case_expr_owned_collection_value_8060,
           control_string_arms_spec_28_3_arm_meet) {
    JitResult r = runAotChecked(prog(
        "      var u := (case i mod 2 of 0 => \"ab\", _ => \"cd\"); "
        "      s := s + u.Length; "));
    expectNoLeak(r, "200\n");
}

// A BORROW meet over two named owned locals (K3.13: an unmarked read is a
// borrow arm; K7.2). The binding views; `a` and `b` keep their obligations.
// A wrong owned classification here would dispose the selected local twice.
BEZEL_TEST(case_expr_owned_collection_value_8060,
           control_borrow_meet_of_named_locals_spec_28_3_1_k7_2) {
    JitResult r = runAotChecked(prog(
        "      var a := MkA(); "
        "      var b := new OwningArray<Cell>(); "
        "      var u := (case i mod 2 of 0 => a, _ => b); "
        "      s := s + u.Count + a.Count; "));
    expectNoLeak(r, "150\n");
}

// The K7.3 rejection is unchanged: an owned `new` arm beside an unmarked
// named-local read (a borrow arm) is a mixed meet.
BEZEL_TEST(case_expr_owned_collection_value_8060,
           control_mixed_owned_and_borrow_arms_rejected_spec_28_3_1_k7_3) {
    JitResult r = runAotChecked(prog(
        "      var a := MkA(); "
        "      var u := (case i mod 2 of 0 => a, _ => new OwningArray<Cell>()); "
        "      s := s + u.Count; "));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticText(r).find(
                          "the arms of this if/case expression mix an owned "
                          "value with a borrow") != std::string::npos);
}
