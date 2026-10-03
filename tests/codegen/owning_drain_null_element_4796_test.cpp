// Codegen — fix #4796 (§29.1 / §14.4, v0.313; stdlib §4.5 / §4.13): a null
// ELEMENT of an `OwningArray<R?>` drained through the `IOwningEnumerable<R?>`
// interface must reach the consumer as a null element, not raise
// InvalidOperationException.
//
// §14.4 (v0.313): "Nullable class types and interfaces are admitted. The
// owning-type instantiation rule checks the *underlying* type, not the
// nullable-wrapper" — so `OwningArray<R?>` is a valid collection and a null
// slot is a legitimately yielded value. §29.1: "after a successful `MoveNext`,
// `TakeCurrent()` transfers ownership of the most-recently-yielded value to
// the caller ... A second take without an intervening advance, or a take
// before the first advance / after exhaustion, raises
// `InvalidOperationException`."
//
// THE DEFECT. The builtin drain frames (`wrapArrayAsIOwningEnumerator` and the
// LinkedList sibling) kept the pending element in a single slot and used
// `pending == null` as the "nothing to take" sentinel in TakeCurrent, so an
// advance that yielded a null element looked like a take-before-advance and
// raised. The fix is an explicit `advanced` flag (set by a successful
// MoveNext, cleared by TakeCurrent / exhaustion); the null `pending` is no
// longer an IOE signal. The §29.1 raising shapes are unchanged and pinned
// below (second take, before-advance, after-exhaustion).
//
// Every row loops 200x inside the program and prints exact stdout; the
// destructor counter `W._n` makes disposal observable (an advanced-but-untaken
// pending must be disposed exactly once, §29.1).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// (Never put a C++ `//` comment inside these literals: it is Bezel source.)
std::string program(const std::string& members, const std::string& roundBody) {
    return std::string(
               "module M4796; "
               "class W "
               "  public static _n: int := 0; "
               "  public Id: int; "
               "  public constructor(id: int) Id := id; end constructor; "
               "  destructor _n := _n + 1; end destructor; "
               "end W; "
               "class C ") +
           members +
           "  public static method Round(): int " + roundBody + " end Round; "
           "  public static method Main(): int "
           "    var total := 0; "
           "    for var i in 0..<200 do total := total + Round(); end; "
           "    Console.WriteLine(total); "
           "    Console.WriteLine(W._n); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

const char* kCount =
    "  public static method Count(src: IOwningEnumerable<W?>): int "
    "    var n := 0; "
    "    for var x in src do n := n + 1; _ := x; end; "
    "    return n; "
    "  end Count; ";

void expectOut(const std::string& src, const char* want) {
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(want));
}

std::string viaInterface(const char* adds) {
    return std::string(
               "    using var a := new OwningArray<W?>(); ") + adds +
           "    using var va := a.Drain(); "
           "    return Count(va); ";
}

}  // namespace

// The issue's exact repro (probe3): 2 live + 1 null element, through the
// interface -> Count == 3 every round; the 2 live elements are disposed by the
// loop's discharge (400 total).
BEZEL_TEST(OwningDrainNullElement_4796, issue_repro_interface_drain_counts_null) {
    expectOut(program(kCount,
                      viaInterface("a.Add(new W(1)); a.Add(null); a.Add(new W(3)); ")),
              "600\n400\n");
}

BEZEL_TEST(OwningDrainNullElement_4796, null_first) {
    expectOut(program(kCount,
                      viaInterface("a.Add(null); a.Add(new W(2)); a.Add(new W(3)); ")),
              "600\n400\n");
}

BEZEL_TEST(OwningDrainNullElement_4796, null_last) {
    expectOut(program(kCount,
                      viaInterface("a.Add(new W(1)); a.Add(new W(2)); a.Add(null); ")),
              "600\n400\n");
}

BEZEL_TEST(OwningDrainNullElement_4796, all_null) {
    expectOut(program(kCount, viaInterface("a.Add(null); a.Add(null); a.Add(null); ")),
              "600\n0\n");
}

// Control: no null element (always worked).
BEZEL_TEST(OwningDrainNullElement_4796, non_null_control) {
    expectOut(program(kCount,
                      viaInterface("a.Add(new W(1)); a.Add(new W(2)); a.Add(new W(3)); ")),
              "600\n600\n");
}

// Control: the direct `for ... in a.Drain()` loop (never went through the
// frame) agrees with the interface path.
BEZEL_TEST(OwningDrainNullElement_4796, direct_for_loop_drain_control) {
    expectOut(program("",
                      "    using var a := new OwningArray<W?>(); "
                      "    a.Add(new W(1)); a.Add(null); a.Add(new W(3)); "
                      "    var n := 0; "
                      "    for var x in a.Drain() do n := n + 1; _ := x; end; "
                      "    return n; "),
              "600\n400\n");
}

// Generic `T?` instantiation through a generic function.
BEZEL_TEST(OwningDrainNullElement_4796, generic_nullable_instantiation) {
    expectOut(program("  public static method CountG<T>(src: IOwningEnumerable<T?>): int "
                      "    where T: class "
                      "    var n := 0; "
                      "    for var x in src do n := n + 1; _ := x; end; "
                      "    return n; "
                      "  end CountG; ",
                      "    using var a := new OwningArray<W?>(); "
                      "    a.Add(new W(1)); a.Add(null); a.Add(new W(3)); "
                      "    using var va := a.Drain(); "
                      "    return CountG<W>(va); "),
              "600\n400\n");
}

// §29.1: a second take without an intervening advance STILL raises IOE, also
// when the taken element was null (the flag, not the null, is the guard).
BEZEL_TEST(OwningDrainNullElement_4796, double_take_of_null_element_still_raises) {
    expectOut(program("",
                      "    using var a := new OwningArray<W?>(); "
                      "    a.Add(null); "
                      "    using var view := a.Drain(); "
                      "    using var it := view.GetOwningEnumerator(); "
                      "    _ := it.MoveNext(); "
                      "    _ := it.TakeCurrent(); "
                      "    var r := 0; "
                      "    try "
                      "      _ := it.TakeCurrent(); "
                      "    catch "
                      "      InvalidOperationException => r := 1; "
                      "    end; "
                      "    return r; "),
              "200\n0\n");
}

// §29.1: take before the first advance / after exhaustion still raise IOE with
// a null element in the collection.
BEZEL_TEST(OwningDrainNullElement_4796, before_advance_and_after_exhaustion_still_raise) {
    expectOut(program("",
                      "    using var a := new OwningArray<W?>(); "
                      "    a.Add(null); "
                      "    using var view := a.Drain(); "
                      "    using var it := view.GetOwningEnumerator(); "
                      "    var r := 0; "
                      "    try _ := it.TakeCurrent(); catch "
                      "      InvalidOperationException => r := r + 1; end; "
                      "    _ := it.MoveNext(); "
                      "    _ := it.TakeCurrent(); "
                      "    if it.MoveNext() then return 100; end; "
                      "    try _ := it.TakeCurrent(); catch "
                      "      InvalidOperationException => r := r + 1; end; "
                      "    return r; "),
              "400\n0\n");
}

// §29.1: an advanced-but-untaken pending value is disposed by the enumerator's
// Dispose (no leak); the remaining element is disposed with the array: both
// elements disposed exactly once per round (400 total).
BEZEL_TEST(OwningDrainNullElement_4796, advanced_but_untaken_pending_disposed_once) {
    expectOut(program("",
                      "    using var a := new OwningArray<W?>(); "
                      "    a.Add(new W(1)); a.Add(new W(2)); "
                      "    using var view := a.Drain(); "
                      "    do "
                      "      using var it := view.GetOwningEnumerator(); "
                      "      _ := it.MoveNext(); "
                      "    end; "
                      "    return a.Count; "),
              "200\n400\n");
}

// §29.1: an advanced-but-untaken NULL pending is a no-op to dispose.
BEZEL_TEST(OwningDrainNullElement_4796, advanced_but_untaken_null_pending_is_noop) {
    expectOut(program("",
                      "    using var a := new OwningArray<W?>(); "
                      "    a.Add(null); a.Add(new W(2)); "
                      "    using var view := a.Drain(); "
                      "    do "
                      "      using var it := view.GetOwningEnumerator(); "
                      "      _ := it.MoveNext(); "
                      "    end; "
                      "    return a.Count; "),
              "200\n200\n");
}
