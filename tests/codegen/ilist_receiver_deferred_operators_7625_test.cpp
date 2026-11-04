// #7625 (stdlib §5 / §4.1, spec §5.1, v0.355): the DEFERRED stdlib §5
// operators on a borrowing `IList<T>` / `IReadOnlyList<T>` receiver.
//
// THE RULE. stdlib §5 declares the operators on `IEnumerable<T>`:
//   "public extension IEnumerable<T>
//      public method Where(predicate: (T) -> bool): IEnumerable<T>;
//      public method Select<U>(selector: (T) -> U): IEnumerable<U>;
//      ...
//      public method Reverse(): IEnumerable<T>;
//      public method Take(count: int): IEnumerable<T>;
//      ...
//      public method Concat(other: IEnumerable<T>): IEnumerable<T>;"
// and, in the `where T: hashable` block, `Distinct()` and `ToSet(): owned
// Set<T>`. stdlib §4.1: `IReadOnlyCollection<out T>: IEnumerable<T>`,
// `IReadOnlyList<out T>: IReadOnlyCollection<T>`, `IList<T>:
// IReadOnlyList<T>, ICollection<T>` — so every operator applies to both list
// interfaces. Neither declares an instance `Reverse()`, so `l.Reverse()` is
// the LAZY §5 operator (it must not reverse the backing array in place).
//
// THE DEFECT. After #7544 these were left UNTYPED on a list-interface
// receiver and codegen lowered none of them: `var w := l.Take(2).ToArray()`
// and `Console.WriteLine(l.Select(f).Count())` were a #2434 / #3079 warning
// plus a DROPPED statement; `l.Skip(1).First()` printed 0; a `for` over
// `l.Where(p)` was "not yet implemented"; `l.Reverse()` mutated the array.
//
// THE FIX routes the receiver through the existing lazy-source machinery
// (`lowerToLazyEnumerator` wraps it as the same BORROWING lazy array source
// as `Array<T>`), so the receiver is never disposed and the lazy frames are
// released by the pipeline exactly once.
//
// Every runtime row is runAotChecked (generated-code ASan + LSan) and loops
// the operators 100x, so a per-iteration leak or double free fails the row.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

void expectCheckedStdout(const std::string& program, const char* expected) {
    JitResult r = runAotChecked(program);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(expected));
}

bool rejectedWith(const std::string& program, const char* needle) {
    JitResult r = runAot(program);
    if (r.ran || !r.stdoutText.empty()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// The nine operators the issue names, each over a list-interface PARAMETER
// (a borrow), consumed by every terminal shape: Count, Sum, First, a
// materializer (ToArray / ToSet under `using var`), a `for` over the lazy
// result, and a `for` over a lazy result bound to a local. 100 calls, then
// the backing array is read back unchanged.
//
// a = [5,3,8,1,3]: Where>2 Count 4; Select*2 Sum 40; Take(2) First 5;
// Skip(1) Count 4; Distinct Count 4; OrderBy First 1; Concat Count 10;
// Reverse First 3; ToSet Count 4; Where>3 ToArray Count 2; for over
// Select+1 25; for over a bound Skip(1).Take(2) 11 — 113 per call.
BEZEL_TEST(ilist_receiver_deferred_operators_7625,
           issue_operators_on_ilist_and_ireadonlylist_spec_stdlib_5) {
    expectCheckedStdout(
        "module M; import System; import System.Linq;\n"
        "class P\n"
        "  public static method Run(l: IList<int>, r: IReadOnlyList<int>): int\n"
        "    var acc := 0;\n"
        "    acc := acc + l.Where(lambda(x) => x > 2).Count();\n"
        "    acc := acc + r.Select(lambda(x) => x * 2).Sum();\n"
        "    acc := acc + l.Take(2).First();\n"
        "    acc := acc + r.Skip(1).Count();\n"
        "    acc := acc + l.Distinct().Count();\n"
        "    acc := acc + r.OrderBy(lambda(x) => x).First();\n"
        "    acc := acc + l.Concat(r).Count();\n"
        "    acc := acc + r.Reverse().First();\n"
        "    using var s := l.ToSet();\n"
        "    acc := acc + s.Count;\n"
        "    using var w := r.Where(lambda(x) => x > 3).ToArray();\n"
        "    acc := acc + w.Count;\n"
        "    for var x in l.Select(lambda(x) => x + 1) do acc := acc + x; end;\n"
        "    var q := r.Skip(1).Take(2);\n"
        "    for var x in q do acc := acc + x; end;\n"
        "    return acc;\n"
        "  end Run;\n"
        "  public static method Main(): int\n"
        "    var a: Array<int> := [5, 3, 8, 1, 3];\n"
        "    var total := 0;\n"
        "    for var i in 1..100 do total := total + P.Run(a, a); end;\n"
        "    Console.WriteLine(P.Run(a, a));\n"
        "    Console.WriteLine(total);\n"
        "    Console.WriteLine($\"{a[0]},{a[4]},{a.Count}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n",
        "113\n11300\n5,3,5\n");
}

// The rest of the deferred surface that reaches the same lazy source
// (TakeWhile, SkipWhile, OrderByDescending, Intersect, Except, GroupBy,
// ToDictionary), and the runtime-managed (`string`) and class (`Box`)
// element kinds, whose slots the borrowing source must neither retain nor
// release.
//
// a = [5,3,8,1,3]: TakeWhile>2 3; SkipWhile>2 2; OrderByDescending First 8;
// Intersect 4; Except 0; GroupBy(mod 2) 2; ToDictionary over [5,3,8,1]
// Count 4 + d[80]=9; strings ["ab","c","def"]: Where Length>1 ToArray 2,
// Select Length Sum 6, for over Reverse 6; boxes [3,1,2]: Where v>1 Count 2,
// Select v Sum 6, OrderBy v First 1 — 55 per call.
BEZEL_TEST(ilist_receiver_deferred_operators_7625,
           remaining_operators_and_element_kinds_spec_stdlib_5) {
    expectCheckedStdout(
        "module M; import System; import System.Linq;\n"
        "class Box\n"
        "  public v: int;\n"
        "  public constructor(x: int) v := x; end constructor;\n"
        "end Box;\n"
        "class P\n"
        "  public static method Run(l: IList<int>, r: IReadOnlyList<int>,\n"
        "      sl: IList<string>, bl: IReadOnlyList<Box>, u: IList<int>): int\n"
        "    var acc := 0;\n"
        "    acc := acc + l.TakeWhile(lambda(x) => x > 2).Count();\n"
        "    acc := acc + r.SkipWhile(lambda(x) => x > 2).Count();\n"
        "    acc := acc + l.OrderByDescending(lambda(x) => x).First();\n"
        "    acc := acc + l.Intersect(r).Count();\n"
        "    acc := acc + r.Except(l).Count();\n"
        "    acc := acc + l.GroupBy(lambda(x) => x mod 2).Count();\n"
        "    using var d := u.ToDictionary(lambda(x) => x * 10, lambda(x) => x + 1);\n"
        "    acc := acc + d.Count + d[80];\n"
        "    using var ss := sl.Where(lambda(s) => s.Length > 1).ToArray();\n"
        "    acc := acc + ss.Count;\n"
        "    acc := acc + sl.Select(lambda(s) => s.Length).Sum();\n"
        "    for var s in sl.Reverse() do acc := acc + s.Length; end;\n"
        "    acc := acc + bl.Where(lambda(b) => b.v > 1).Count();\n"
        "    acc := acc + bl.Select(lambda(b) => b.v).Sum();\n"
        "    acc := acc + bl.OrderBy(lambda(b) => b.v).First().v;\n"
        "    return acc;\n"
        "  end Run;\n"
        "  public static method Main(): int\n"
        "    var a: Array<int> := [5, 3, 8, 1, 3];\n"
        "    var ua: Array<int> := [5, 3, 8, 1];\n"
        "    var sa: Array<string> := [\"ab\", \"c\", \"def\"];\n"
        "    using var ba := new OwningArray<Box>();\n"
        "    ba.Add(new Box(3)); ba.Add(new Box(1)); ba.Add(new Box(2));\n"
        "    var total := 0;\n"
        "    for var i in 1..100 do total := total + P.Run(a, a, sa, ba, ua); end;\n"
        "    Console.WriteLine(P.Run(a, a, sa, ba, ua));\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n",
        "55\n5500\n");
}

// stdlib §4.1 declares no instance `Reverse()` on the list interfaces, so the
// §5 lazy operator is the call's only meaning: the backing array is NOT
// reversed in place (on base `l.Reverse()` reached the in-place `Array<T>`
// mutator through the list-interface bridge). Also the `?.` spelling of a
// deferred operator on a nullable list interface, null and non-null.
//
// Per iteration: rv[0] 1 + x?.Where(>3)?.Count() 2 + n?… ?? 1000 + the sum
// over the bound lazy Reverse 17 = 1020; 100 iterations.
BEZEL_TEST(ilist_receiver_deferred_operators_7625,
           reverse_is_lazy_and_null_conditional_spec_stdlib_5_spec_7_5) {
    expectCheckedStdout(
        "module M; import System; import System.Linq;\n"
        "class P\n"
        "  public static method Main(): int\n"
        "    var a: Array<int> := [5, 3, 8, 1];\n"
        "    var l: IList<int> := a;\n"
        "    var x: IReadOnlyList<int>? := a;\n"
        "    var n: IList<int>? := null;\n"
        "    var acc := 0;\n"
        "    for var i in 1..100 do\n"
        "      using var rv := l.Reverse().ToArray();\n"
        "      acc := acc + rv[0] + (x?.Where(lambda(v) => v > 3)?.Count() ?? -100)\n"
        "        + (n?.Where(lambda(v) => v > 3)?.Count() ?? 1000);\n"
        "      var lz := l.Reverse();\n"
        "      for var v in lz do acc := acc + v; end;\n"
        "    end;\n"
        "    Console.WriteLine(acc);\n"
        "    Console.WriteLine($\"{a[0]},{a[1]},{a[2]},{a[3]}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n",
        "102000\n5,3,8,1\n");
}

// spec §5.1: a deferred operator's result is a lazy `IEnumerable<T>`, and no
// §5.1 conversion takes a sequence to a primitive. Typing the list-interface
// operators must not turn this line into an accepted program (on base it was
// rejected only because the call produced no value); the same gate covers
// the concrete `Array<T>` receiver, which accepted it and bound the sequence
// pointer into the `int` slot.
const char* kLazyToPrimitive =
    "a deferred stdlib §5 operator yields a lazy `IEnumerable<T>` sequence, "
    "which converts to no primitive type";

std::string lazyToPrimProg(const std::string& stmt) {
    return "module M; import System; import System.Linq; "
           "class P public static method Main(): int "
           "  var a: Array<int> := [5, 3, 8, 1]; "
           "  var l: IList<int> := a; var r: IReadOnlyList<int> := a; " +
           stmt + " return 0; end Main; end P;";
}

BEZEL_TEST(ilist_receiver_deferred_operators_7625,
           lazy_result_into_int_local_rejected_on_ireadonlylist_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        lazyToPrimProg("var q: int := r.Where(lambda(x) => x > 3);"),
        kLazyToPrimitive));
}

BEZEL_TEST(ilist_receiver_deferred_operators_7625,
           lazy_result_into_string_local_rejected_on_ilist_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        lazyToPrimProg("var q: string := l.Skip(1);"), kLazyToPrimitive));
}

BEZEL_TEST(ilist_receiver_deferred_operators_7625,
           lazy_result_into_int_local_rejected_on_array_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        lazyToPrimProg("var q: int := a.Where(lambda(x) => x > 3); "
                       "Console.WriteLine(q);"),
        kLazyToPrimitive));
}

// Control: the lazy result still binds to its own declared type.
BEZEL_TEST(ilist_receiver_deferred_operators_7625,
           lazy_result_into_ienumerable_local_still_accepted_spec_stdlib_5) {
    expectCheckedStdout(
        lazyToPrimProg("var c := 0; "
                       "for var i in 1..100 do "
                       "  var q: IEnumerable<int> := l.Where(lambda(x) => x > 3); "
                       "  for var v in q do c := c + v; end; "
                       "end; "
                       "var q2: IEnumerable<int> := r.Where(lambda(x) => x > 3); "
                       "Console.WriteLine(c); Console.WriteLine(q2.Count());"),
        "1300\n2\n");
}
