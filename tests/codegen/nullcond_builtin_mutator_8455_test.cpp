// Spec-derived regression test for issue #8455 -- `n?.Sort(cmp)` on a
// non-null `Array<W>?` left the array UNSORTED.
//
// THE DEFECT. The `?.` spelling of an IN-PLACE collection mutator --
// `n?.Sort(cmp)`, `n?.Sort()`, `n?.Reverse()`, `n?.Clear()`, `n?.Add(x)` --
// lowered to NOTHING: the mutator never ran, so the array kept its original
// order and the program exited 0 with no diagnostic. MEASURED on the fix's
// parent (issue #8455's repro): the array printed `b c a` where §7.5 requires
// `a b c`.
//
// WHY. Sema types the `?.` call by LIFTING the `.` twin's type to nullable
// (§7.5: "The result type is always nullable"). A mutator that returns
// NOTHING has no type to lift -- §4.7 makes the unit type non-nullable -- so
// the `?.` call stayed UNTYPED and codegen's `?.`-builtin lowering declined
// the whole call.
//
// spec §7.5 "Null-conditional access" (v0.357): "`x?.M` returns null if `x` is
// null; otherwise it performs the access." and "`?.` short-circuits. The
// receiver is evaluated first; if it is null the access does not occur and
// nothing to its right in the chain is evaluated."
// spec §7.6 (v0.357): the result of a procedure-typed call is `()`.
// stdlib §4.4 `Array<T>` (v0.357): the imperative-verb form "mutates the
// receiver in place and returns nothing" -- `Sort(comparer)`, `Reverse()`,
// `Clear()`, `Add(item: T)`.
//
// The rows below are the ISSUE'S OWN repro first, then the twins of the same
// shape (`?.Sort()` without a comparer, `?.Reverse()`, `?.Clear()`,
// `?.Add(9)`), the null-receiver no-op (§7.5's short-circuit), and the `.`
// spelling as the control that already worked.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotOut;

namespace {

// The issue's repro shape: a record element, a `Array<W>?` receiver that is
// NOT null, and a comparer lambda.
std::string prog(const std::string& body) {
    return std::string(
               "module M; "
               "record W(K: string, V: int); "
               "class C "
               "  public static method Main(): int ") +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The `Array<W>` the repro sorts, and the `Array<W>?` that holds it.
const char* kArrW =
    "    var a: Array<W> := [new W(\"b\", 1), new W(\"c\", 2), new W(\"a\", 3)]; "
    "    var n: Array<W>? := a; ";

// The keys of `a`, in order.
const char* kPrintKeys =
    "    for var e in a do Console.Write($\"{e.K} \"); end; "
    "    Console.WriteLine(); ";

}  // namespace

// The issue's own repro: the annotated comparer lambda.
BEZEL_TEST(nullcond_builtin_mutator_8455, sort_with_comparer) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            std::string(kArrW) +
            "    n?.Sort(lambda(x: W, y: W): int => x.K.CompareTo(y.K)); " +
            kPrintKeys)),
        std::string("a b c "));
}

// The same call with the receiver spelled `Array<W>` (not nullable): the
// `?.` twin must see the SAME receiver value the `.` spelling sees.
BEZEL_TEST(nullcond_builtin_mutator_8455, sort_with_comparer_control) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            std::string(
                "    var a: Array<W> := [new W(\"b\", 1), new W(\"c\", 2), new W(\"a\", 3)]; "
                "    a.Sort(lambda(x: W, y: W): int => x.K.CompareTo(y.K)); ") +
            kPrintKeys)),
        std::string("a b c "));
}

// The no-comparer `?.Sort()` twin.
BEZEL_TEST(nullcond_builtin_mutator_8455, sort_without_comparer) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var a: Array<int> := [3, 1, 2]; "
            "    var n: Array<int>? := a; "
            "    n?.Sort(); "
            "    for var e in a do Console.Write($\"{e} \"); end; "
            "    Console.WriteLine(); ")),
        std::string("1 2 3 "));
}

// The `?.Reverse()` twin: the in-place reverse, not a copy.
BEZEL_TEST(nullcond_builtin_mutator_8455, reverse) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var a: Array<int> := [1, 2, 3]; "
            "    var n: Array<int>? := a; "
            "    n?.Reverse(); "
            "    for var e in a do Console.Write($\"{e} \"); end; "
            "    Console.WriteLine(); ")),
        std::string("3 2 1 "));
}

// The `?.Clear()` twin: the array is emptied IN PLACE.
BEZEL_TEST(nullcond_builtin_mutator_8455, clear) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var a: Array<int> := [1, 2, 3]; "
            "    var n: Array<int>? := a; "
            "    n?.Clear(); "
            "    Console.WriteLine($\"{a.Count} \"); ")),
        std::string("0 "));
}

// The `?.Add(9)` twin: the item is appended IN PLACE.
BEZEL_TEST(nullcond_builtin_mutator_8455, add) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var a: Array<int> := [1, 2, 3]; "
            "    var n: Array<int>? := a; "
            "    n?.Add(9); "
            "    for var e in a do Console.Write($\"{e} \"); end; "
            "    Console.WriteLine(); ")),
        std::string("1 2 3 9 "));
}

// §7.5's short-circuit: a NULL receiver performs no call, and the array
// behind a DIFFERENT (non-null) receiver is untouched.
BEZEL_TEST(nullcond_builtin_mutator_8455, null_receiver_is_a_no_op) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var a: Array<int> := [3, 1, 2]; "
            "    var n: Array<int>? := null; "
            "    n?.Sort(); "
            "    n?.Reverse(); "
            "    n?.Clear(); "
            "    n?.Add(9); "
            "    for var e in a do Console.Write($\"{e} \"); end; "
            "    Console.WriteLine(); ")),
        std::string("3 1 2 "));
}

// The `?.` mutator and the `.` mutator agree on the SAME receiver: the
// `?.` spelling is not a copy of the array.
BEZEL_TEST(nullcond_builtin_mutator_8455, nullcond_mutates_the_same_array) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var a: Array<int> := [3, 1, 2]; "
            "    var n: Array<int>? := a; "
            "    n?.Sort(); "
            "    a.Reverse(); "
            "    for var e in a do Console.Write($\"{e} \"); end; "
            "    Console.WriteLine(); ")),
        std::string("3 2 1 "));
}
