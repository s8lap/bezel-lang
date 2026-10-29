// #7544 (spec §7.5 + stdlib §5 / §4.1, v0.350): a `?.` call to a stdlib §5
// operator (Sum / First / Any / Contains / ToArray) on an `IList<T>?`,
// `IReadOnlyList<T>?` or `IEnumerable<T>?` receiver must type as the `.` form's
// result lifted to nullable, and lower, so the enclosing `??` reads it.
//
// THE RULES.
//   §7.5   "`x?.M` returns null if `x` is null; otherwise it performs the
//          access. The result type is always nullable." A null receiver skips
//          the call and its arguments.
//   stdlib §5  the operators are `extension IEnumerable<T>` members, and
//          `IList<T>` / `IReadOnlyList<T>` implement `IEnumerable<T>` (§4.1).
//          `Contains(value: T): bool` is the §5 operator; on `IList<T>` it is
//          also declared by `ICollection<T>` (§4.1).
//
// THE DEFECT. The typeCall `?.` twin types a §5 operator by re-entering the
// `.` typing on a placeholder receiver. That typing only knew the lazy sentinel
// and an `IEnumerable<T>` head, so on an `IList<T>` / `IReadOnlyList<T>` head
// every operator stayed untyped (the `.` form too: codegen lowered the value
// without a sema type), and `Contains` was typed on NO receiver. The `?.` call
// then had no type to lift, and the enclosing statement was dropped behind the
// #2434 / #3079 "failed to lower" warning. `.ToArray()` on a list interface
// additionally produced no value in codegen (#2457).
//
// Every row is runAotChecked (generated-code ASan + LSan) and loops the access.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; import System.Linq; "
           "class P "
           "  public static method Main(): int " +
           body + " return 0; end Main; end P;";
}

// The operator x receiver matrix for one interface head.
std::string matrix(const std::string& head) {
    return prog(
        "  var a: Array<int> := [1, 2, 3]; "
        "  var x: " + head + "<int>? := a; "
        "  var n: " + head + "<int>? := null; "
        "  Console.WriteLine(x?.Sum() ?? -1); "
        "  Console.WriteLine(n?.Sum() ?? -1); "
        "  Console.WriteLine(x?.First() ?? -1); "
        "  Console.WriteLine(n?.First() ?? -1); "
        "  Console.WriteLine(x?.Any() ?? false); "
        "  Console.WriteLine(n?.Any() ?? true); "
        "  Console.WriteLine(x?.Contains(2) ?? false); "
        "  Console.WriteLine(x?.Contains(9) ?? true); "
        "  Console.WriteLine(n?.Contains(2) ?? true); "
        "  var tx := x?.ToArray(); "
        "  var tn := n?.ToArray(); "
        "  Console.WriteLine(tx?.Count ?? -1); "
        "  Console.WriteLine(tn?.Count ?? -1); "
        "  var acc := 0; "
        "  for var i in 1..100 do "
        "    var t := x?.ToArray(); "
        "    acc := acc + (x?.Sum() ?? 0) + (n?.Sum() ?? 1000) "
        "           + (t?.Count ?? 0); "
        "  end; "
        "  Console.WriteLine(acc); ");
}

// 6 -1 1 -1 True True True False True 3 -1, then 100 * (6 + 1000 + 3).
const char* kExpected =
    "6\n-1\n1\n-1\nTrue\nTrue\nTrue\nFalse\nTrue\n3\n-1\n100900\n";

void expectMatrix(const std::string& head) {
    JitResult r = runAotChecked(matrix(head));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(kExpected));
}

}  // namespace

BEZEL_TEST(null_cond_linq_iface_7544, ilist_operators_spec_7_5) {
    expectMatrix("IList");
}

// `IReadOnlyList<T>` declares no `Contains` of its own (§4.1): the call is the
// §5 operator (#7557).
BEZEL_TEST(null_cond_linq_iface_7544, ireadonlylist_operators_spec_7_5) {
    expectMatrix("IReadOnlyList");
}

BEZEL_TEST(null_cond_linq_iface_7544, ienumerable_operators_spec_7_5) {
    expectMatrix("IEnumerable");
}

// The `.` form: `.ToArray().Count` on a list interface produced no value.
BEZEL_TEST(null_cond_linq_iface_7544, dot_form_to_array_count_spec_stdlib_5) {
    JitResult r = runAotChecked(prog(
        "  var a: Array<int> := [1, 2, 3]; "
        "  var il: IList<int> := a; "
        "  var ro: IReadOnlyList<int> := a; "
        "  Console.WriteLine(il.ToArray().Count); "
        "  Console.WriteLine(ro.ToArray().Count); "
        "  var t := il.ToArray(); "
        "  Console.WriteLine(t[2]); "
        "  var s := 0; "
        "  for var i in 1..100 do "
        "    s := s + il.ToArray().Count + ro.ToArray().Count; "
        "  end; "
        "  Console.WriteLine(s); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n3\n3\n600\n");
}

// §7.5 Evaluation: a null receiver skips the argument expression too.
BEZEL_TEST(null_cond_linq_iface_7544, null_receiver_skips_argument_spec_7_5) {
    JitResult r = runAotChecked(
        "module M; import System; import System.Linq; "
        "class P "
        "  public static method Next(c: Array<int>): int "
        "    c[0] := c[0] + 1; return 2; "
        "  end Next; "
        "  public static method Main(): int "
        "    var a: Array<int> := [1, 2, 3]; "
        "    var cnt: Array<int> := [0]; "
        "    var ro: IReadOnlyList<int>? := a; "
        "    var nr: IReadOnlyList<int>? := null; "
        "    var x := nr?.Contains(Next(cnt)) ?? false; "
        "    var y := ro?.Contains(Next(cnt)) ?? false; "
        "    Console.WriteLine(x); Console.WriteLine(y); "
        "    Console.WriteLine(cnt[0]); "
        "    return 0; "
        "  end Main; end P;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\nTrue\n1\n");
}

// ---------------------------------------------------------------------------
// FIX-FORWARD (review findings A and B): the typing above admits ONLY the §5
// operators codegen lowers on a borrowing list interface. Everything else stays
// as LOUD as on base — never a degraded #3079 warning, never a runtime
// InvalidOperationException. runAot synthesizes an Error diagnostic for a
// runtime crash too, so each row asserts the diagnostic TEXT and that nothing
// ran.
// ---------------------------------------------------------------------------

namespace {

std::string listProg(const std::string& body) {
    return prog("  var a: Array<int> := [5, 3, 8, 1]; "
                "  var l: IList<int> := a; "
                "  var r: IReadOnlyList<int> := a; " + body);
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

const char* kNoValue = "produced no value, and no dispatch arm emitted any code";

}  // namespace

// stdlib §5 `First` declares arity 0 or 1; three arguments is the #3726 error,
// not a silent drop (finding A).
BEZEL_TEST(null_cond_linq_iface_7544, wrong_arity_first_on_ireadonlylist_spec_stdlib_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        listProg("Console.WriteLine(r.First(1, 2, 3)); "),
        "wrong number of arguments for stdlib §5 LINQ operator `First`"));
}

BEZEL_TEST(null_cond_linq_iface_7544, wrong_arity_count_on_ilist_spec_stdlib_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        listProg("Console.WriteLine(l.Count(1, 2)); "),
        "wrong number of arguments for stdlib §5 LINQ operator `Count`"));
}

BEZEL_TEST(null_cond_linq_iface_7544, wrong_arity_contains_on_ilist_spec_stdlib_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        listProg("Console.WriteLine(l.Contains(1, 2)); "),
        "wrong number of arguments for stdlib §5 LINQ operator `Contains`"));
}

BEZEL_TEST(null_cond_linq_iface_7544, wrong_arity_to_array_on_ilist_spec_stdlib_5) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        listProg("Console.WriteLine(l.ToArray(5).Count); "),
        "wrong number of arguments for stdlib §5 LINQ operator `ToArray`"));
}

// Finding A: a deferred operator bound to a declared non-lazy type.
BEZEL_TEST(null_cond_linq_iface_7544, where_into_int_local_stays_rejected_on_ireadonlylist) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        listProg("var q: int := r.Where(lambda(x: int) => x > 1); "), kNoValue));
}

// Finding B: deferred operators on a list interface are NOT lowered; they were
// a loud "produced no value" on base and must stay one (the review measured a
// runtime InvalidOperationException on the widened typing).
BEZEL_TEST(null_cond_linq_iface_7544, where_then_count_stays_rejected_on_ireadonlylist) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        listProg("var w := r.Where(lambda(x: int) => x > 1); "
                 "Console.WriteLine(w.Count()); "),
        kNoValue));
}

BEZEL_TEST(null_cond_linq_iface_7544, select_then_first_stays_rejected_on_ilist) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        listProg("var w := l.Select(lambda(x: int) => x * 2); "
                 "Console.WriteLine(w.First()); "),
        kNoValue));
}

// `ToSet` has no list-interface lowering either (the widened typing degraded
// it to a #3079 warning and `Count` 0).
BEZEL_TEST(null_cond_linq_iface_7544, to_set_stays_rejected_on_ilist) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        listProg("var s := l.ToSet(); Console.WriteLine(s.Count); "), kNoValue));
}
