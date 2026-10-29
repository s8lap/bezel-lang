// #7016 (spec §7.5 + stdlib §4.1, v0.350): `il?.Count` and `il?.Contains(x)` on
// an `IList<T>?` (and `il?.Count` on an `IReadOnlyList<T>?`) must type and lower.
//
// THE RULES.
//   §7.5   "`x?.M` returns null if `x` is null; otherwise it performs the
//          access. The result type is always nullable."
//   stdlib §4.1  `IReadOnlyCollection<T>` declares `Count: int`, inherited by
//          `IReadOnlyList<T>` and `IList<T>`; `ICollection<T>` declares
//          `Contains(item: T): bool`, inherited by `IList<T>`.
//
// THE DEFECT. resolveNullCondMember typed `c?.Count` only on the concrete
// collection / owning heads (the #6024 head predicates), never on the borrowing
// list interfaces, and typed no `?.` call to `Contains` on a declaration-less
// interface head at all. Both accesses stayed untyped, so codegen dropped the
// enclosing statement behind the #2434 "failed to lower" warning (the local held
// 0 / nothing printed).
//
// Every row is runAotChecked (generated-code ASan + LSan) and loops the access.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; import System.Linq; "
           "class P "
           "  public static method Main(): int " +
           body + " return 0; end Main; end P;";
}

}  // namespace

// The issue's `Count` shape: an `int?` slot initialised from `il?.Count`.
BEZEL_TEST(null_cond_list_iface_7016, count_property_spec_7_5) {
    JitResult r = runAotChecked(prog(
        "  var a: Array<int> := [1, 2, 3]; "
        "  var il: IList<int>? := a; "
        "  var ro: IReadOnlyList<int>? := a; "
        "  var nl: IList<int>? := null; "
        "  var nr: IReadOnlyList<int>? := null; "
        "  var c: int? := il?.Count; "
        "  Console.WriteLine(c ?? -1); "
        "  var sum := 0; "
        "  for var i in 1..60 do "
        "    sum := sum + (il?.Count ?? 100) + (ro?.Count ?? 100) "
        "           + (nl?.Count ?? 1000) + (nr?.Count ?? 1000); "
        "  end; "
        "  Console.WriteLine(sum); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // per iteration: 3 + 3 + 1000 + 1000 = 2006; 60 iterations = 120360.
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n120360\n");
}

// The issue's `Contains` shape, reading the result (null receiver -> null).
BEZEL_TEST(null_cond_list_iface_7016, contains_call_spec_7_5) {
    JitResult r = runAotChecked(prog(
        "  var a: Array<int> := [1, 2, 3]; "
        "  var il: IList<int>? := a; "
        "  var nl: IList<int>? := null; "
        "  var b: bool? := il?.Contains(2); "
        "  Console.WriteLine(b ?? false); "
        "  Console.WriteLine(il?.Contains(9) ?? true); "
        "  Console.WriteLine(nl?.Contains(2) ?? true); "
        "  var hits := 0; "
        "  for var i in 1..60 do "
        "    if il?.Contains(i) ?? false then hits := hits + 1; end; "
        "    if nl?.Contains(i) ?? false then hits := hits + 1000; end; "
        "  end; "
        "  Console.WriteLine(hits); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // il holds 1,2,3: i = 1,2,3 hit => 3; the null receiver never searches.
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\nTrue\n3\n");
}

// §7.5 Evaluation: a null receiver skips the argument expression too.
BEZEL_TEST(null_cond_list_iface_7016, null_receiver_skips_argument_spec_7_5) {
    JitResult r = runAotChecked(
        "module M; import System; import System.Linq; "
        "class P "
        "  public static method Next(c: Array<int>): int "
        "    c[0] := c[0] + 1; return 2; "
        "  end Next; "
        "  public static method Main(): int "
        "    var a: Array<int> := [1, 2, 3]; "
        "    var cnt: Array<int> := [0]; "
        "    var il: IList<int>? := a; "
        "    var nl: IList<int>? := null; "
        "    var x := nl?.Contains(Next(cnt)) ?? false; "
        "    var y := il?.Contains(Next(cnt)) ?? false; "
        "    Console.WriteLine(x); Console.WriteLine(y); "
        "    Console.WriteLine(cnt[0]); "
        "    return 0; "
        "  end Main; end P;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\nTrue\n1\n");
}
