// Marked arm transfer `move a` over a builtin-COLLECTION local
// (issue #7907, the `new Array` half; spec §28.3.4 producers, §28.3 K3.4/K7.4,
// §20 *Ownership*, stdlib §3.1, v0.357).
//
// THE RULE. §28.3 K3.4 admits "the marked arm transfer: `move x` as an
// if-expression branch or an expression-`case` arm result" for "a named owned
// binding"; §20 *Ownership* states the class-typed arm-meet ("owned arms meet
// to owned ... the marked branch `move x` transfers, consuming on the path
// where its branch is selected and leaving the binding per-path consumed after
// the construct"). `Array<T>` is a class (stdlib §4.4) and `new Array<int>()`
// is an Owned producer (§28.3.4), so
//
//   var a := new Array<int>(); a.Add(1);
//   return (if c then move a else new Array<int>());   // in `owned Array<int>`
//
// is the spec's own equalization written over a collection.
//
// THE BUG (#7907). It was rejected: "the arm transfer marker `move a` is
// supported only on a plain `var` owned local of class or interface type in
// this version" -- the #2080 first-landing bound, whose operand test read
// `TypeRef::declaration->kind`, which a declaration-less builtin collection
// (`Array`, `Dictionary`, `OwningArray`, ...) never has. Its scope-end
// registration was also a class-vtable `dynamicDispose`, which would misread a
// collection handle, so the fix routes a collection through the null-safe
// collection disposal.
//
// The plain-position twin `TakeA(move a)` was admitted all along (positive
// control, row 1). The annotated-literal half of #7907 (`var a: Array<int> :=
// [1,2]; TakeA(move a)`) is #7980's and is NOT claimed here.
//
// per §28.3 K3.4/K7.4, §20, stdlib §3.1, v0.357

#include <string>

#include "support/aot_support.hpp"  // runAotChecked -- AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

std::string prog(const std::string& members, const std::string& body) {
    return "module M; import System; import System.Collections.Generic; "
           "public class Program " + members +
           " public static method Main(): int " + body +
           " end Main; end Program;";
}

bool hasErrorContaining(const JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

const char* kBoundNeedle = "supported only on a plain `var` owned local";

void expectOut(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kBoundNeedle));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// Row 1 (admitting twin / positive control): the PLAIN-position transfer.
BEZEL_TEST(collection_local_arm_transfer_7907,
           spec_28_3_k3_4_new_array_local_plain_move_into_owned_param_control) {
    auto r = runAotChecked(prog(
        "public static method Take(owned a: Array<int>): int "
        "  return a.Count; "
        "end Take; ",
        "var t := 0; "
        "for var i in 0..<100 do "
        "  var a := new Array<int>(); a.Add(1); a.Add(2); "
        "  t += Take(move a); "
        "end for; "
        "Console.WriteLine(t); return 0;"));
    expectOut(r, "200\n");
}

// Row 2: THE ISSUE'S REPRO (a12 `Mk2`): the arm transfer in a `return`
// position, selected on even iterations, so BOTH paths run 50 times each --
// the selected path moves `a` out (50 elements total), the not-selected path
// reclaims `a` at scope end (a leak/double-free either way fails the checked
// oracle).
BEZEL_TEST(collection_local_arm_transfer_7907,
           spec_20_ownership_if_expr_move_arm_over_new_array_local_returns_owned) {
    auto r = runAotChecked(prog(
        "public static method Mk2(c: bool): owned Array<int> "
        "  var a := new Array<int>(); a.Add(1); "
        "  return (if c then move a else new Array<int>()); "
        "end Mk2; ",
        "var t := 0; "
        "for var i in 0..<100 do t += Mk2(i mod 2 = 0).Count; end for; "
        "Console.WriteLine(t); return 0;"));
    expectOut(r, "50\n");
}

// Row 3: the arm transfer feeds a local binding; the SOURCE is the arm that is
// not selected on odd iterations (`move a` is on the else side here), and the
// selected-fresh path leaves `a` to its scope-end reclaim.
BEZEL_TEST(collection_local_arm_transfer_7907,
           spec_28_3_k3_4_move_arm_on_else_side_binds_owned_local) {
    auto r = runAotChecked(prog("",
        "var t := 0; "
        "for var i in 0..<100 do "
        "  var a := new Array<int>(); a.Add(7); a.Add(8); a.Add(9); "
        "  var x := (if i mod 2 = 0 then new Array<int>() else move a); "
        "  t += x.Count; "
        "end for; "
        "Console.WriteLine(t); return 0;"));
    expectOut(r, "150\n");
}

// Row 4: reads of the TRANSFERRED value are the transferred data (not a fresh
// empty collection): sums the moved elements.
BEZEL_TEST(collection_local_arm_transfer_7907,
           spec_28_3_k3_4_move_arm_value_carries_the_moved_elements) {
    auto r = runAotChecked(prog("",
        "var t := 0; "
        "for var i in 0..<100 do "
        "  var a := new Array<int>(); a.Add(i); a.Add(2); "
        "  var x := (if true then move a else new Array<int>()); "
        "  t += x[0] + x[1]; "
        "end for; "
        "Console.WriteLine(t); return 0;"));
    expectOut(r, "5150\n");
}

// Row 5: the expression-`case` spelling (§28.3.4 producers: "the result of an
// expression-position `case`" meets its arms; the marked arm is admitted).
BEZEL_TEST(collection_local_arm_transfer_7907,
           spec_28_3_4_expression_case_move_arm_over_collection_local) {
    auto r = runAotChecked(prog("",
        "var t := 0; "
        "for var i in 0..<100 do "
        "  var a := new Array<int>(); a.Add(1); a.Add(2); "
        "  var x := (case i mod 2 of "
        "    0 => move a, "
        "    _ => new Array<int>()); "
        "  t += x.Count; "
        "end for; "
        "Console.WriteLine(t); return 0;"));
    expectOut(r, "100\n");
}

// Row 6: the other builtin collection heads share the same declaration-less
// shape: Dictionary, Set (plain), OwningArray (owning).
BEZEL_TEST(collection_local_arm_transfer_7907,
           spec_28_3_k3_4_move_arm_over_dictionary_and_set_locals) {
    auto r = runAotChecked(prog(
        "public static method MkD(c: bool): owned Dictionary<int, int> "
        "  var d := new Dictionary<int, int>(); d.Add(1, 2); d.Add(3, 4); "
        "  return (if c then move d else new Dictionary<int, int>()); "
        "end MkD; "
        "public static method MkS(c: bool): owned Set<int> "
        "  var s := new Set<int>(); s.Add(5); "
        "  return (if c then move s else new Set<int>()); "
        "end MkS; ",
        "var t := 0; "
        "for var i in 0..<100 do "
        "  t += MkD(i mod 2 = 0).Count + MkS(i mod 2 = 0).Count; "
        "end for; "
        "Console.WriteLine(t); return 0;"));
    expectOut(r, "150\n");
}

// Row 7 (admitting twin): a user CLASS local under the same marker was
// admitted all along; its counted disposals are the control for rows 2-6.
// Only the SELECTED arm is evaluated: an even i builds one Widget (`w`, moved
// out), an odd i builds two (`w` plus the fresh arm), so 50*1 + 50*2 = 150
// constructions, every one destructed exactly once.
BEZEL_TEST(collection_local_arm_transfer_7907,
           spec_20_ownership_if_expr_move_arm_over_class_local_control) {
    auto r = runAotChecked(
        "module M; import System; "
        "public class Widget "
        "  public static _n: int := 0; "
        "  public constructor() end constructor; "
        "  destructor Widget._n := Widget._n + 1; end destructor; "
        "end Widget; "
        "public class Program "
        "  public static method Mk(c: bool): owned Widget "
        "    var w := new Widget(); "
        "    return (if c then move w else new Widget()); "
        "  end Mk; "
        "  public static method Main(): int "
        "    for var i in 0..<100 do var x := Mk(i mod 2 = 0); end for; "
        "    Console.WriteLine(Widget._n); return 0; "
        "  end Main; "
        "end Program;");
    expectOut(r, "150\n");
}

// Row 8 (still-rejected, K7.4): after the join the collection local is
// per-path consumed, so reading it is rejected exactly as for a class local.
// Pins that the fix admits the TRANSFER only, not a use-after-maybe-move.
BEZEL_TEST(collection_local_arm_transfer_7907,
           spec_28_3_k7_4_read_after_arm_transfer_of_collection_still_rejected) {
    auto r = runAot(prog("",
        "var a := new Array<int>(); a.Add(1); "
        "var x := (if true then move a else new Array<int>()); "
        "return a.Count + x.Count;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_FALSE(hasErrorContaining(r, kBoundNeedle));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Row 9: a NULLABLE collection local (`Array<int>?`) takes the same marker --
// the class-local gate never excluded a nullable local, and the scope-end
// disposal is null-guarded -- and the arm-meet result is itself nullable.
BEZEL_TEST(collection_local_arm_transfer_7907,
           spec_28_3_k3_4_move_arm_over_nullable_collection_local) {
    auto r = runAotChecked(prog("",
        "var t := 0; "
        "for var i in 0..<100 do "
        "  var a: Array<int>? := new Array<int>(); a!.Add(1); "
        "  var x := (if i mod 2 = 0 then move a else new Array<int>()); "
        "  t += x!.Count; "
        "end for; "
        "Console.WriteLine(t); return 0;"));
    expectOut(r, "50\n");
}
