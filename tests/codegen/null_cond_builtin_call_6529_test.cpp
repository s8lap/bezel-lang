// #6529 (stdlib §4.4 / §5 / §9.6, spec §7.5, v0.338): the CALL spelling of a
// null-conditional access (`a?.Count()`, `src?.First()`, `a?.ToArray()` …) on a
// declaration-less built-in head (Array, Dictionary, string, Span, a lazy
// IEnumerable) is a VALUE. Pre-fix it lowered to nothing: the enclosing
// return / store was dropped and the program exited 0 answering wrongly,
// signalled only by the `internal: ... failed to lower` #2491/#2961 backstop.
//
// THE RULE. §7.5: "`x?.M` returns null if `x` is null; otherwise it performs the
// access. The result type is always nullable." No nullability precondition on
// the receiver. stdlib §4.4 `Array<T>` and §5's `extension IEnumerable<T>`
// declare `Count()`, `First()`, `Sum()`, `Any()`, `Contains(x)` and
// `ToArray()`, so each `?.` call names a real member and is `T?`.
//
// WHERE IT WAS LOST (same class as #6512, which fixed the IGrouping receiver).
//   sema    — the member-access typing of a built-in / LINQ call is keyed on a
//             MemberAccessExpr callee; a `?.` call's callee is a
//             NullCondMemberExpr, so most such calls stayed UNTYPED.
//   codegen — lowerNullCondCall requires a MethodSymbol; a built-in member has
//             none, so it returned nullptr before any lowering.
//   owned   — `var b := a?.ToArray()` was not recognised as a fresh array
//             materializer local, so its backing leaked.
//
// Every value row is runAot and asserts STDOUT byte-for-byte (exit 0 alone is
// not an oracle: the pre-fix behavior exits 0). Owned-result rows are
// runAotChecked, looped 100x.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::failedAtCompileTimeWithRealLocation;

namespace {

std::string prog(const std::string& body) {
    return "module M; class C "
           "  public static method Main(): int " +
           body + " return 0; end Main; end C;";
}

bool hasErrorContaining(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// Issue repro A (verbatim shape): concrete Array<int>.
BEZEL_TEST(null_cond_builtin_call_6529, array_count_call_spec_7_5_4_4) {
    JitResult r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: Array<int> := [1, 2, 3, 4]; "
        "    return a?.Count() ?? -1; "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);
}

// Issue repro B: a lazy IEnumerable<int> source (`Where`).
BEZEL_TEST(null_cond_builtin_call_6529, lazy_where_count_call_spec_7_5_5) {
    JitResult r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: Array<int> := [1, 2, 3, 4]; "
        "    return a.Where(lambda(x: int) => x > 1)?.Count() ?? -1; "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

BEZEL_TEST(null_cond_builtin_call_6529, receiver_matrix_stdout_spec_7_5) {
    JitResult r = runAot(prog(
        "  var a: Array<int> := [5, 2, 3, 4]; "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Add(\"a\", 1); d.Add(\"b\", 2); "
        "  var s: string := \"abc\"; "
        "  var sp := a.AsSpan(); "
        "  var w := a.Where(lambda(x: int) => x > 2); "
        "  Console.WriteLine(a?.Count() ?? -1); "
        "  Console.WriteLine(d?.Count() ?? -1); "
        "  Console.WriteLine(s?.Count() ?? -1); "
        "  Console.WriteLine(sp?.IndexOf(3) ?? -1); "
        "  Console.WriteLine(w?.Count() ?? -1); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "4\n2\n3\n2\n3\n");
}

BEZEL_TEST(null_cond_builtin_call_6529, null_receivers_yield_null_spec_7_5) {
    JitResult r = runAot(prog(
        "  var a: Array<int>? := null; "
        "  var d: Dictionary<string, int>? := null; "
        "  var s: string? := null; "
        "  Console.WriteLine(a?.Count() ?? -1); "
        "  Console.WriteLine(d?.Count() ?? -2); "
        "  Console.WriteLine(s?.Count() ?? -3); "
        "  Console.WriteLine(a?.First() ?? -4); "
        "  Console.WriteLine(a?.Sum() ?? -5); "
        "  Console.WriteLine(a?.Any() ?? false); "
        "  Console.WriteLine(a?.Contains(3) ?? false); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "-1\n-2\n-3\n-4\n-5\nFalse\nFalse\n");
}

BEZEL_TEST(null_cond_builtin_call_6529, other_members_stdout_spec_7_5_5) {
    JitResult r = runAot(prog(
        "  var a: Array<int> := [5, 2, 3, 4]; "
        "  var e: Array<int> := new Array<int>(); "
        "  Console.WriteLine(a?.First() ?? -1); "
        "  Console.WriteLine(a?.Sum() ?? -1); "
        "  Console.WriteLine(a?.Any() ?? false); "
        "  Console.WriteLine(e?.Any() ?? true); "
        "  Console.WriteLine(a?.Contains(3) ?? false); "
        "  Console.WriteLine(a?.Contains(9) ?? true); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n14\nTrue\nFalse\nTrue\nFalse\n");
}

// The `?.` result is `int?`: §7.11 forbids `+` over it. Pins the diagnostic
// that SURVIVES the fix (the `internal:` backstop is the needle that dies).
BEZEL_TEST(null_cond_builtin_call_6529, count_call_is_nullable_int_spec_7_11) {
    JitResult r = runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: Array<int> := [1, 2, 3, 4]; "
        "    var n := 1; "
        "    return n + a?.Count(); "
        "  end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasErrorContaining(
        r, "operator `+` is not defined on a nullable value type"));
}

// Owned result: `ToArray()` mints a fresh array on the non-null path. Looped
// 100x under the checked (ASan/LSan) oracle; a one-shot leak would hide.
BEZEL_TEST(null_cond_builtin_call_6529, toarray_owned_result_no_leak_spec_28_3) {
    JitResult r = runAotChecked(prog(
        "  var a: Array<int> := [1, 2, 3, 4]; "
        "  var sum := 0; var i := 0; "
        "  while i < 100 do "
        "    var b := a?.ToArray(); "
        "    if b <> null then sum := sum + b.Count; end; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(sum); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "400\n");
}

// Owned lazy temporary in the receiver: `a.Where(..)?.ToArray()` — the Where
// iterator must be disposed exactly once (non-null path) and the array too.
BEZEL_TEST(null_cond_builtin_call_6529, lazy_receiver_toarray_no_leak_spec_28_3) {
    JitResult r = runAotChecked(prog(
        "  var a: Array<int> := [1, 2, 3, 4]; "
        "  var sum := 0; var i := 0; "
        "  while i < 100 do "
        "    var b := a.Where(lambda(x: int) => x > 1)?.ToArray(); "
        "    if b <> null then sum := sum + b.Count; end; "
        "    sum := sum + (a.Where(lambda(x: int) => x > 2)?.Count() ?? 0); "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(sum); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "500\n");
}

// Null receiver: no allocation, `b` stays null, nothing to dispose.
BEZEL_TEST(null_cond_builtin_call_6529, null_receiver_toarray_no_leak_spec_7_5) {
    JitResult r = runAotChecked(prog(
        "  var a: Array<int>? := null; "
        "  var hits := 0; var i := 0; "
        "  while i < 100 do "
        "    var b := a?.ToArray(); "
        "    if b <> null then hits := hits + 1; end; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(hits); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "0\n");
}
