// Spec-derived regression tests for issue #7980: a collection-literal local
// bound with an ANNOTATED type (`var a: Array<int> := [1];`) was classified a
// BORROW, so `Take(move a)` into an `owned` parameter was rejected with
// §28.8's "a borrow cannot be passed where an `owned` value is required".
//
// The spec sentences that decide the outcome (v0.357):
//
//   §28.3.4, the producers table: "Collection literal (`[1, 2, 3]`)
//   targeting a reference type (`Array<T>`, `OwningArray<T>`, `Set<T>`,
//   `Dictionary<K, V>`, etc.) | **Owned**".
//
//   §28.3.3: "`var x := expr;` takes the ownership state of `expr`".
//
//   §15.4: "A collection literal has no standalone type. It is well-typed
//   only in a context that provides the target type" -- so the literal's
//   produced type is the local's DECLARED type, and sema records no
//   `exprType` for the literal node itself (why the local fell to Borrowed).
//
// => `a` owns the literal, `move a` is an owned value (§28.8.1), and the
// `owned` parameter may take it, exactly as it takes the `new Array<int>()`
// twin's.
//
// ORACLES. The accept rows use `runAotChecked` (compiled, linked, run; the
// ASan leg runs under BEZEL_AOT_FORCE_SANITIZE=address). On the pre-fix
// compiler the issue's program is rejected at compile time, so its row is
// RED there. The loop rows repeat the transfer 200 times so a per-iteration
// leak of the moved literal is visible to LeakSanitizer. The reject rows
// assert the specific diagnostic.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The §28.8 owned sink: it adopts and disposes what it is given.
constexpr const char* kOwnedArraySink =
    "  public static method Take(x: owned Array<int>): int "
    "    using var hold := move x; "
    "    return hold.Count; "
    "  end Take; ";

std::string program(const char* sink, const char* body) {
    return std::string("module M; import System; class C ") + sink +
           "  public static method Main(): int " + body +
           "  end Main; "
           "end C;";
}

}  // namespace

// THE ISSUE'S OWN REPRO (§28.3.4 producers row, §28.8.1, v0.357). RED on the
// pre-fix compiler: "a borrow cannot be passed where an `owned` value is
// required".
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           annotated_literal_moved_into_owned_param_spec_28_3_4) {
    auto r = runAotChecked(program(
        kOwnedArraySink,
        "    var a: Array<int> := [1]; "
        "    var n := Take(move a); "
        "    Console.WriteLine(n); "
        "    return 0; "));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// The same transfer 200 times: the moved literal is disposed once by the
// sink, never leaked and never freed again by the caller's scope end
// (§28.3.4, §28.8, §15.4 "An `owned` parameter takes the aggregate by
// transfer", v0.357). RED on the pre-fix compiler (rejected).
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           annotated_literal_moved_in_loop_disposed_once_spec_28_8) {
    auto r = runAotChecked(program(
        kOwnedArraySink,
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var a: Array<int> := [1, 2, 3]; "
        "      acc := acc + Take(move a); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc); "
        "    return 0; "));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("600\n"));
}

// A second head named by the producers row: `Dictionary<K, V>` (§28.3.4,
// v0.357). RED on the pre-fix compiler (rejected).
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           annotated_dictionary_literal_moved_into_owned_param_spec_28_3_4) {
    auto r = runAotChecked(program(
        "  public static method Take(x: owned Dictionary<string, int>): int "
        "    using var hold := move x; "
        "    return hold.Count; "
        "  end Take; ",
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var d: Dictionary<string, int> := [\"a\": 1, \"b\": 2]; "
        "      acc := acc + Take(move d); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc); "
        "    return 0; "));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("400\n"));
}

// The admitting twin: the `new` spelling of the same producers row was
// admitted all along. GREEN on both sides -- it shows the annotated row is
// refused before the fix for the classification alone.
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           new_form_twin_moved_into_owned_param_spec_28_3_4) {
    auto r = runAotChecked(program(
        kOwnedArraySink,
        "    var a := new Array<int>(); "
        "    a.Add(1); "
        "    var n := Take(move a); "
        "    Console.WriteLine(n); "
        "    return 0; "));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// The owned sink is not widened: the UNMARKED read of the now-owned local
// into an `owned` parameter is §22.3's marker error, the one the `new` twin
// draws (§22.3, v0.357). Before the fix this program drew §28.8's borrow
// diagnostic instead, so the row is RED there on its needle.
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           annotated_literal_unmarked_into_owned_param_rejected_spec_22_3) {
    auto r = runAot(program(
        kOwnedArraySink,
        "    var a: Array<int> := [1]; "
        "    var n := Take(a); "
        "    Console.WriteLine(n); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "write `M(move a)`"));
}

// A read after the transfer is a use of a moved-from local (§28.3.3,
// v0.357).
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           annotated_literal_read_after_move_rejected_spec_28_3_3) {
    auto r = runAot(program(
        kOwnedArraySink,
        "    var a: Array<int> := [1]; "
        "    var n := Take(move a); "
        "    Console.WriteLine(a.Count); "
        "    return n; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "owned value in `a` was already moved out at this point"));
}

// A transfer on SOME paths only takes the verdict its `new`-form twin takes
// (`var a := new Array<int>(); if c then Take(move a); end if;` is rejected
// with the same diagnostic): the literal local's exemption from the
// scope-exit obligation does not cover a maybe-consumed state (§28.3.3,
// v0.357; recorded interpretation, #7980).
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           annotated_literal_conditional_move_matches_new_twin_spec_28_3_3) {
    auto r = runAot(program(
        "  public static method Take(x: owned Dictionary<string, int>): int "
        "    using var hold := move x; "
        "    return hold.Count; "
        "  end Take; "
        "  public static method Run(c: bool): int "
        "    var d: Dictionary<string, int> := [\"a\": 1, \"b\": 2]; "
        "    var n := 0; "
        "    if c then n := Take(move d); end if; "
        "    return n; "
        "  end Run; ",
        "    Console.WriteLine(C.Run(true)); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "owned value in `d` would be dropped here without disposal"));
}

// ---------------------------------------------------------------------------
// narrow #7980 (§15.4 literal-tree ownership rule, §28.3.4, v0.357): the
// Owned classification is recorded only for a PROVABLY FLAT literal.
//
// §15.4: "The fresh nodes of a collection literal -- nested literal nodes,
// fresh producer elements (`new` expressions and `owned`-returning calls in
// element position), and any element expression whose arm-meet
// classification is owned ... -- form an aggregate per maximal
// borrowing-family subtree. Each aggregate is owned by whatever owns its
// subtree's root: ... an `owned` parameter or return through which the
// container transfers". Freeing that aggregate at an `owned` receiver is
// #2699's unimplemented stage, so a literal local that may carry such a node
// keeps its pre-#7980 classification and `move` of it into an `owned`
// parameter is still refused -- a DIVERGENCE from §28.3.4's row (which has no
// element condition), recorded on #7980 and tracked by its follow-up.
//
// Each of the six element forms below is an Owned producer per §28.3.4
// (`x!` of a fresh producer, `move x`, `move obj.f`, a range slice, an owned
// `??` meet, a bind atom whose body is `new`). The second cut of this fix
// excluded fresh elements by a blacklist of forms that missed all six: the
// program compiled and leaked the element on every iteration (LeakSanitizer).
// These rows are RED on that cut (the program compiles) and GREEN with the
// whitelist.
namespace {

constexpr const char* kRankTypes =
    "interface IRank method Rank(): int; end IRank; "
    "class Pt: IRank "
    "  public v: int; "
    "  public constructor(v: int) this.v := v; end constructor; "
    "  public method Rank(): int return v; end Rank; "
    "end Pt; ";

constexpr const char* kOwnedRankArraySink =
    "  public static method Take(x: owned Array<IRank>): int "
    "    using var hold := move x; "
    "    return hold[0].Rank(); "
    "  end Take; ";

constexpr const char* kBorrowIntoOwnedSink =
    "a borrow cannot be passed where an `owned` value is required";

std::string programWith(const char* types, const char* members,
                        const char* body) {
    return std::string("module M; import System; ") + types + "class C " +
           members + "  public static method Main(): int " + body +
           "  end Main; "
           "end C;";
}

// The loop every row shares: the literal local is declared and moved 200
// times, so an admitted program's per-iteration leak is visible.
std::string movedInLoop(const char* setup, const char* decl) {
    return std::string("    var i := 0; var acc := 0; ") + setup +
           "    while i < 200 do " + decl +
           "      acc := acc + Take(move arr); "
           "      i := i + 1; "
           "    end while; "
           "    Console.WriteLine(acc); "
           "    return 0; ";
}

}  // namespace

// `[new Pt(3)!]`: §28.3.4 -- `x!` takes `x`'s classification, and a fresh
// producer's `!` is owned.
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           literal_with_bang_of_new_element_not_movable_spec_15_4) {
    auto r = runAot(programWith(
        kRankTypes, kOwnedRankArraySink,
        movedInLoop("", "      var arr: Array<IRank> := [new Pt(3)!]; ")
            .c_str()));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kBorrowIntoOwnedSink));
}

// `[move p]`: a marked transfer of an owned local into the element slot.
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           literal_with_moved_local_element_not_movable_spec_15_4) {
    auto r = runAot(programWith(
        kRankTypes, kOwnedRankArraySink,
        movedInLoop("", "      var p := new Pt(3); "
                        "      var arr: Array<IRank> := [move p]; ")
            .c_str()));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kBorrowIntoOwnedSink));
}

// `[move b.f]`: §28.3.4 -- `move obj.f` is Owned.
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           literal_with_moved_field_element_not_movable_spec_15_4) {
    auto r = runAot(programWith(
        "class Pt "
        "  public v: int; "
        "  public constructor(v: int) this.v := v; end constructor; "
        "end Pt; "
        "class Box "
        "  public owned f: Pt?; "
        "  public constructor() this.f := null; end constructor; "
        "end Box; ",
        "  public static method Take(x: owned Array<Pt?>): int "
        "    using var hold := move x; "
        "    return hold.Count; "
        "  end Take; ",
        movedInLoop("    using var b := new Box(); ",
                    "      b.f := new Pt(i); "
                    "      var arr: Array<Pt?> := [move b.f]; ")
            .c_str()));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kBorrowIntoOwnedSink));
}

// `[src[0..2]]`: §28.3.4 -- a range slice is Owned.
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           literal_with_range_slice_element_not_movable_spec_15_4) {
    auto r = runAot(programWith(
        "",
        "  public static method Take(x: owned Array<Array<int>>): int "
        "    using var hold := move x; "
        "    return hold.Count; "
        "  end Take; ",
        movedInLoop("    var src := new Array<int>(); "
                    "    src.Add(1); src.Add(2); src.Add(3); ",
                    "      var arr: Array<Array<int>> := [src[0..2]]; ")
            .c_str()));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kBorrowIntoOwnedSink));
}

// `[C.M() ?? new Pt(3)]`: §28.3.4 -- `??` is the meet of its operands, both
// owned here.
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           literal_with_owned_coalesce_element_not_movable_spec_15_4) {
    const std::string members =
        std::string("  public static method M(): owned Pt? return null; "
                    "end M; ") +
        kOwnedRankArraySink;
    auto r = runAot(programWith(
        kRankTypes, members.c_str(),
        movedInLoop("",
                    "      var arr: Array<IRank> := [C.M() ?? new Pt(3)]; ")
            .c_str()));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kBorrowIntoOwnedSink));
}

// `[(var t := 3; new Pt(t))]`: §28.3.4 -- a bind atom takes its body's
// classification.
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           literal_with_bind_atom_new_body_element_not_movable_spec_15_4) {
    auto r = runAot(programWith(
        kRankTypes, kOwnedRankArraySink,
        movedInLoop(
            "",
            "      var arr: Array<IRank> := [(var t := 3; new Pt(t))]; ")
            .c_str()));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kBorrowIntoOwnedSink));
}

// §15.4 resolution 4 / §28.7, v0.357: "drop-on-overwrite (§28.7) frees the
// whole displaced aggregate after RHS evaluation". Codegen's displaced-
// collection disposal frees the displaced container, not a nested literal's
// interior nodes, so the store over a NESTED literal local stays rejected
// (the pre-#7980 verdict). The second cut widened the drop-on-overwrite
// coverage to every literal local, ungated: this program compiled and leaked
// the nested `[1]` on every iteration. RED on that cut, GREEN here.
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           nested_literal_local_reassigned_new_rejected_spec_15_4_res_4) {
    auto r = runAot(program(
        "",
        "    var acc := 0; var i := 0; "
        "    while i < 200 do "
        "      var a: Array<Array<int>> := [[1]]; "
        "      a := new Array<Array<int>>(); "
        "      acc := acc + a.Count + 1; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "owned value in `a` would be dropped here without disposal"));
}

// The whitelist's VALUE-slot arm: an element slot of primitive type cannot
// hold a heap node whatever expression fills it, so a conditional, a call
// and an arithmetic element all keep the literal movable (§28.3.4 producers
// row, v0.357). 200 iterations under LeakSanitizer. RED on the pre-fix
// compiler (the borrow diagnostic) -- and on the second cut, whose blacklist
// refused every conditional element.
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           value_slot_literal_any_element_form_moved_spec_28_3_4) {
    auto r = runAotChecked(program(
        kOwnedArraySink,
        "    var i := 0; var acc := 0; "
        "    while i < 200 do "
        "      var arr: Array<int> := "
        "          [(if i > 3 then 1 else 2), Math.Max(i, 2), i + 1]; "
        "      acc := acc + Take(move arr); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc); "
        "    return 0; "));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("600\n"));
}

// The whitelist's BORROWED-REFERENCE arm: unmarked named bindings of class
// type are borrowed elements, and §15.4 says "borrowed elements ... are never
// members and are never freed with it". The array is transferred; `p` and `q`
// stay with their own `using var` owners (no double free, no leak).
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           borrowed_class_binding_elements_literal_moved_spec_15_4) {
    auto r = runAotChecked(programWith(
        kRankTypes, kOwnedRankArraySink,
        "    var i := 0; var acc := 0; "
        "    while i < 200 do "
        "      using var p := new Pt(2); "
        "      using var q := new Pt(5); "
        "      var arr: Array<IRank> := [p, q]; "
        "      acc := acc + Take(move arr); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc); "
        "    return 0; "));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("400\n"));
}

// An EMPTY literal over an owning-family head has no element to consume, so
// it is movable; the elements added afterwards are owned by the collection
// and freed by the sink's cascade (§28.3.4 producers row, §15.4, v0.357).
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           empty_owning_array_literal_filled_then_moved_spec_28_3_4) {
    auto r = runAotChecked(programWith(
        kRankTypes,
        "  public static method Take(x: owned OwningArray<Pt>): int "
        "    using var hold := move x; "
        "    return hold.Count; "
        "  end Take; ",
        "    var i := 0; var acc := 0; "
        "    while i < 200 do "
        "      var arr: OwningArray<Pt> := []; "
        "      arr.Add(new Pt(i)); "
        "      acc := acc + Take(move arr); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc); "
        "    return 0; "));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n"));
}

// An EMPTY literal over a two-argument head fills neither slot: only the slot
// TYPES are vetted (`string` and `int`, both value slots), so the local is
// Owned and movable (§28.3.4 producers row, v0.357).
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           empty_dictionary_literal_filled_then_moved_spec_28_3_4) {
    auto r = runAotChecked(programWith(
        "",
        "  public static method Take(x: owned Dictionary<string, int>): int "
        "    using var hold := move x; "
        "    return hold.Count; "
        "  end Take; ",
        "    var i := 0; var acc := 0; "
        "    while i < 200 do "
        "      var d: Dictionary<string, int> := []; "
        "      d.Add(\"k\", i); "
        "      acc := acc + Take(move d); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc); "
        "    return 0; "));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n"));
}

// An `Object` slot BOXES a value-typed element, and the box is a heap node
// the receiver of the transfer does not free: on the second cut of this fix
// `var a: Array<Object> := [n, s]; Take(move a);` compiled and leaked 4800
// bytes in 200 allocations over 200 iterations (LeakSanitizer; #7298 is the
// box-release defect). The whitelist declines every `Object` slot, so the
// local keeps its Borrowed classification and the move is rejected, as on
// the base (§15.4, §28.8, v0.357).
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           object_slot_literal_not_movable_spec_15_4) {
    auto r = runAot(programWith(
        "",
        "  public static method Take(x: owned Array<Object>): int "
        "    using var hold := move x; "
        "    return hold.Count; "
        "  end Take; ",
        "    var i := 0; var acc := 0; "
        "    while i < 200 do "
        "      var s := \"s{i}\"; "
        "      var a: Array<Object> := [i, s]; "
        "      acc := acc + Take(move a); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kBorrowIntoOwnedSink));
}
