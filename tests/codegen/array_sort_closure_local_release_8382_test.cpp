// Codegen — issue #8382: a CLOSURE LOCAL handed to `Array<T>.Sort(comparer)` /
// `Array<T>.Sorted(comparer)` (stdlib §4.4) never released its closure cell.
//
// THE DEFECT. `var cmp: (int, int) -> int := lambda(x: int, y: int) => x - y;`
// `a.Sort(cmp);` leaked the 64-byte `bz.closure` cell on every execution
// (MEASURED: 64 B in 1 allocation one-shot, 6400 B in 100 allocations looped;
// a capturing lambda also leaks its environment box). The comparer parameter
// is PLAIN (stdlib §4.4: `public method Sort(comparer: (T, T) -> int);`, "the
// comparer doesn't consume; it inspects"), so by spec §22.3 ("the `owned`
// modifier is what transfers ownership", v0.357) the position is a BORROW and
// the LOCAL keeps the producer's +1; spec §28.9's function-typed bullet puts
// function values on the runtime-managed tier, whose manager for a closure
// local is the compiler-inserted scope-end release of the cell.
//
// ROOT CAUSE (src/codegen/LlvmBackend_Setup.cpp, `ClosureEscapeWalker::expr`,
// the CallExpr arm): the walk classifies a closure local read as a call
// ARGUMENT as ESCAPED unless a relief covers the callee; #7803 added the
// console's relief and left this sibling open. The sort lowering
// (LlvmBackend_Calls.cpp `m == "Sort"` / `m == "Sorted"` arms) only invokes
// the comparer through a `bz$sortcmp` trampoline and stores nothing, and
// releases only an INLINE lambda literal, so the escape walk is the only
// place the local's discharge can be armed.
//
// FAIL-WITHOUT / PASS-WITH: every leak row below uses the ASan+LSan AOT
// runner (`runAotChecked`), so `ran == false` IS the leak assertion; each is
// looped 100x so a one-shot can never be the oracle. Values are asserted
// through captured stdout.

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

std::string checkedOut(const std::string& prog, bool& ran) {
    auto r = runAotChecked(prog);
    ran = r.ran;
    return r.ran ? r.stdoutText : std::string("<leak-or-asan>");
}

// LSan OFF, generated-code ASan ON: for the controls whose program is
// deliberately outside the relief (a pre-existing, separately-tracked leak),
// so the row asserts "no crash / no double free / right value".
std::string lenientOut(const std::string& prog, bool& ran) {
    auto r = runAotCheckedLenient(prog);
    ran = r.ran;
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t pos = hay.find(needle);
         pos != std::string::npos;
         pos = hay.find(needle, pos + needle.size()))
        ++n;
    return n;
}

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() &&
           s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

const std::string kPre =
    "module M; "
    "class P public static method Main(): int ";

const std::string kPost = " return 0; end Main; end P;";

}  // namespace

// ── THE ISSUE'S REPRO, verbatim (one shot). Pre-fix: LSan "64 byte(s) leaked
// in 1 allocation(s)". (issue #8382; stdlib §4.4, spec §22.3 / §28.9, v0.357)
BEZEL_TEST(array_sort_closure_local_release_8382,
           issue_repro_verbatim_released) {
    bool ran = false;
    auto s = checkedOut(
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    var cmp: (int, int) -> int := lambda(x: int, y: int) => x - y; "
        "    var a: Array<int> := [3, 1, 2]; "
        "    a.Sort(cmp); "
        "    Console.WriteLine(a[0]); "
        "    return 0; "
        "  end Main; "
        "end C;", ran);
    BEZEL_EXPECT_TRUE(ran);   // LeakSanitizer-clean: the FAIL-WITHOUT oracle
    BEZEL_EXPECT_EQ(s, std::string("1\n"));
}

// ── The same program looped, the closure declared inside the body: one cell
// per iteration (pre-fix 6400 B / 100 allocations), released at each scope end.
BEZEL_TEST(array_sort_closure_local_release_8382,
           sort_closure_local_in_loop_released_each_iteration) {
    bool ran = false;
    auto s = checkedOut(
        std::string(kPre) +
            "for var i in 0..<100 do "
            "  var cmp: (int, int) -> int := lambda(x: int, y: int) => x - y; "
            "  var a: Array<int> := [3, 1, 2]; "
            "  a.Sort(cmp); "
            "  Console.WriteLine(a[0]); "
            "end for; "
            "Console.WriteLine(\"DONE\");" +
            kPost, ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(countOccurrences(s, "1\n"), 100);   // sorted ascending
    BEZEL_EXPECT_TRUE(endsWith(s, "DONE\n"));
}

// ── `Sorted(comparer)` (stdlib §4.4) is the same borrow at the same position.
// A DESCENDING comparer, so the value proves the closure really ran.
BEZEL_TEST(array_sort_closure_local_release_8382,
           sorted_closure_local_released) {
    bool ran = false;
    auto s = checkedOut(
        std::string(kPre) +
            "for var i in 0..<100 do "
            "  var cmp: (int, int) -> int := lambda(x: int, y: int) => y - x; "
            "  var a: Array<int> := [3, 1, 2]; "
            "  var t := a.Sorted(cmp); "
            "  Console.WriteLine(t[0]); "
            "end for; "
            "Console.WriteLine(\"DONE\");" +
            kPost, ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(countOccurrences(s, "3\n"), 100);
    BEZEL_EXPECT_TRUE(endsWith(s, "DONE\n"));
}

// ── THE ROW THAT REJECTS A WRONG FIX: the closure is sorted with TWICE and
// CALLED after the sorts. The local still owns the cell, so `cmp(5, 2)` must
// read 3 — a fix that discharged the closure AT the sort would abort with a
// heap-use-after-free / double free here.
BEZEL_TEST(array_sort_closure_local_release_8382,
           sort_twice_then_call_closure_no_use_after_free) {
    bool ran = false;
    auto s = checkedOut(
        std::string(kPre) +
            "for var i in 0..<100 do "
            "  var cmp: (int, int) -> int := lambda(x: int, y: int) => x - y; "
            "  var a: Array<int> := [3, 1, 2]; "
            "  a.Sort(cmp); "
            "  a.Sort(cmp); "
            "  Console.WriteLine(a[0] + cmp(5, 2)); "
            "end for; "
            "Console.WriteLine(\"DONE\");" +
            kPost, ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(countOccurrences(s, "4\n"), 100);   // 1 + 3
    BEZEL_EXPECT_TRUE(endsWith(s, "DONE\n"));
}

// ── A CAPTURING comparer: the cell AND its environment box are released
// (pre-fix 7204 B / 201 allocations). The captured `k` scales the comparer so
// the value proves the environment was live during the sort.
BEZEL_TEST(array_sort_closure_local_release_8382,
           capturing_comparer_cell_and_env_released) {
    bool ran = false;
    auto s = checkedOut(
        std::string(kPre) +
            "var k := 10; "
            "for var i in 0..<100 do "
            "  var cmp: (int, int) -> int := "
            "    lambda(x: int, y: int) => (x - y) * k; "
            "  var a: Array<int> := [3, 1, 2]; "
            "  a.Sort(cmp); "
            "  Console.WriteLine(a[0]); "
            "end for; "
            "Console.WriteLine(\"DONE\");" +
            kPost, ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(countOccurrences(s, "1\n"), 100);
    BEZEL_EXPECT_TRUE(endsWith(s, "DONE\n"));
}

// ── A `string` element type: a different element kind through the same arm.
BEZEL_TEST(array_sort_closure_local_release_8382,
           string_element_comparer_released) {
    bool ran = false;
    auto s = checkedOut(
        std::string(kPre) +
            "for var i in 0..<100 do "
            "  var cmp: (string, string) -> int := "
            "    lambda(x: string, y: string) => x.Length - y.Length; "
            "  var a: Array<string> := [\"ccc\", \"a\", \"bb\"]; "
            "  a.Sort(cmp); "
            "  Console.WriteLine(a[0]); "
            "end for; "
            "Console.WriteLine(\"DONE\");" +
            kPost, ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(countOccurrences(s, "a\n"), 100);
    BEZEL_EXPECT_TRUE(endsWith(s, "DONE\n"));
}

// ── CONTROL (admitting twin): an INLINE lambda literal at the same position
// was already released by the sort lowering itself; it must stay clean, and
// must not be released twice.
BEZEL_TEST(array_sort_closure_local_release_8382,
           control_inline_lambda_literal_still_clean) {
    bool ran = false;
    auto s = checkedOut(
        std::string(kPre) +
            "for var i in 0..<100 do "
            "  var a: Array<int> := [3, 1, 2]; "
            "  a.Sort(lambda(x: int, y: int) => x - y); "
            "  Console.WriteLine(a[0]); "
            "end for; "
            "Console.WriteLine(\"DONE\");" +
            kPost, ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(countOccurrences(s, "1\n"), 100);
    BEZEL_EXPECT_TRUE(endsWith(s, "DONE\n"));
}

// ── CONTROL: a closure that ALSO escapes (stored into an Array of functions
// after the sort) keeps its ESCAPE classification -- the relief must not arm
// the local's scope-end release over a cell another holder still reads. LSan
// is OFF (this program's storing side has its own, separate accounting); the
// generated-code ASan stays armed, so a double free / use-after-free fails.
BEZEL_TEST(array_sort_closure_local_release_8382,
           control_closure_also_stored_no_double_free) {
    bool ran = false;
    auto s = lenientOut(
        std::string(kPre) +
            "for var i in 0..<100 do "
            "  var cmp: (int, int) -> int := lambda(x: int, y: int) => x - y; "
            "  var a: Array<int> := [3, 1, 2]; "
            "  a.Sort(cmp); "
            "  var fs: Array<(int, int) -> int> := []; "
            "  fs.Add(cmp); "
            "  Console.WriteLine(fs[0](9, 4) + a[0]); "
            "end for; "
            "Console.WriteLine(\"DONE\");" +
            kPost, ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(countOccurrences(s, "6\n"), 100);   // 5 + 1
    BEZEL_EXPECT_TRUE(endsWith(s, "DONE\n"));
}

// ── A REASSIGNED closure local sorted with before and after the reassignment:
// both cells are released (the slot's reassignment releases the old value) and
// the final comparer is the one that orders -- no double free, no leak
// (pre-fix 6400 B / 100 allocations).
BEZEL_TEST(array_sort_closure_local_release_8382,
           reassigned_closure_local_sorted_before_and_after) {
    bool ran = false;
    auto s = checkedOut(
        std::string(kPre) +
            "for var i in 0..<100 do "
            "  var cmp: (int, int) -> int := lambda(x: int, y: int) => x - y; "
            "  var a: Array<int> := [3, 1, 2]; "
            "  a.Sort(cmp); "
            "  cmp := lambda(x: int, y: int) => y - x; "
            "  a.Sort(cmp); "
            "  Console.WriteLine(a[0]); "
            "end for; "
            "Console.WriteLine(\"DONE\");" +
            kPost, ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(countOccurrences(s, "3\n"), 100);   // descending
    BEZEL_EXPECT_TRUE(endsWith(s, "DONE\n"));
}

// ── CONTROL: `OwningArray<T>.Sort(comparer)` is NOT the lowering this relief
// was proved against; it is deliberately unchanged (still the conservative
// escape, so no use-after-free is possible). LSan OFF; value + no crash.
// The still-open residual (this and the eager LINQ terminals) is #8432.
BEZEL_TEST(array_sort_closure_local_release_8382,
           control_owningarray_comparer_unchanged_no_crash) {
    bool ran = false;
    auto s = lenientOut(
        "module M; "
        "public class Item "
        "  public v: int; "
        "  public constructor(v: int) this.v := v; end constructor; "
        "end Item; "
        "public class C "
        "  public static method Main(): int "
        "    for var i in 0..<100 do "
        "      var cmp: (Item, Item) -> int := "
        "        lambda(x: Item, y: Item) => x.v - y.v; "
        "      var a := new OwningArray<Item>(); "
        "      a.Add(new Item(3)); "
        "      a.Add(new Item(1)); "
        "      a.Sort(cmp); "
        "      Console.WriteLine(a[0].v); "
        "    end for; "
        "    Console.WriteLine(\"DONE\"); "
        "    return 0; "
        "  end Main; "
        "end C;", ran);
    BEZEL_EXPECT_TRUE(ran);
    BEZEL_EXPECT_EQ(countOccurrences(s, "1\n"), 100);
    BEZEL_EXPECT_TRUE(endsWith(s, "DONE\n"));
}
