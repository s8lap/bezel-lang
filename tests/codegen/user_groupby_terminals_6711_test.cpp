// #6711 (stdlib §5, §7.5, v0.339): a USER method (class, extension, or interface)
// named `GroupBy`, called with `.`, is an ordinary call. The syntactic
// isGroupByCall sites hijacked `u.GroupBy(f).ToArray()` / `.Select(..).Sum()`
// as the stdlib intrinsic. §7.5: "`x.M(...)` ... performs the member access";
// stdlib §5: GroupBy is a member of IEnumerable<T>, not of user types.
// Each user row has its stdlib GroupBy control beside it (runAot, stdout).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {
const char* kUserCls =
    "class U\n  public method GroupBy(f: (int) -> int): Array<int>\n"
    "    return [10, 20, 30];\n  end GroupBy;\nend U;\n";
const char* kExt =
    "class U\nend U;\n"
    "extension U\n  public method GroupBy(f: (int) -> int): Array<int>\n"
    "    return [10, 20, 30];\n  end GroupBy;\nend extension;\n";
const char* kIface =
    "interface IU\n  method GroupBy(f: (int) -> int): Array<int>;\nend IU;\n"
    "class U: IU\n  public method GroupBy(f: (int) -> int): Array<int>\n"
    "    return [10, 20, 30];\n  end GroupBy;\nend U;\n";

std::string mainOf(const char* decl, const char* recvDecl, const std::string& body) {
    return std::string("module M;\n") + decl +
           "class C\n  public static method Main(): int\n" + recvDecl + body +
           "    return 0;\n  end Main;\nend C;\n";
}
std::string run(const std::string& src) {
    JitResult r = runAot(src);
    return r.ran ? r.stdoutText : std::string("<not run>");
}
const char* kU = "    using var u := new U();\n";
const char* kF = "lambda(x: int) => x";
}  // namespace

// using var t := <recv>.GroupBy(f).ToArray()
BEZEL_TEST(user_groupby_terminals_6711, using_var_toarray_spec_stdlib_5) {
    const std::string b = std::string("    using var t := u.GroupBy(") + kF +
                          ").ToArray();\n    Console.WriteLine(t.Count);\n    Console.WriteLine(t[2]);\n";
    BEZEL_EXPECT_EQ(run(mainOf(kUserCls, kU, b)), "3\n30\n");
    BEZEL_EXPECT_EQ(run(mainOf(kExt, kU, b)), "3\n30\n");
    BEZEL_EXPECT_EQ(run(mainOf(kIface, "    using var u: IU := new U();\n", b)), "3\n30\n");
    // Stdlib control.
    BEZEL_EXPECT_EQ(run(mainOf("", "    var a: Array<int> := [1, 2, 3];\n",
                               std::string("    using var t := a.GroupBy(") + kF +
                                   ").ToArray();\n    Console.WriteLine(t.Count);\n")),
                    "3\n");
}

BEZEL_TEST(user_groupby_terminals_6711, plain_var_toarray_spec_stdlib_5) {
    const std::string b = std::string("    var t := u.GroupBy(") + kF +
                          ").ToArray();\n    Console.WriteLine(t.Count);\n    Console.WriteLine(t[0]);\n";
    BEZEL_EXPECT_EQ(run(mainOf(kUserCls, kU, b)), "3\n10\n");
    BEZEL_EXPECT_EQ(run(mainOf(kExt, kU, b)), "3\n10\n");
    BEZEL_EXPECT_EQ(run(mainOf("", "    var a: Array<int> := [1, 2, 3];\n",
                               std::string("    var t := a.GroupBy(") + kF +
                                   ").ToArray();\n    Console.WriteLine(t.Count);\n")),
                    "3\n");
}

BEZEL_TEST(user_groupby_terminals_6711, select_sum_spec_stdlib_5) {
    const std::string b = std::string("    Console.WriteLine(u.GroupBy(") + kF +
                          ").Select(lambda(x: int) => x + 1).Sum());\n";
    BEZEL_EXPECT_EQ(run(mainOf(kUserCls, kU, b)), "63\n");
    BEZEL_EXPECT_EQ(run(mainOf(kExt, kU, b)), "63\n");
    BEZEL_EXPECT_EQ(run(mainOf(kIface, "    using var u: IU := new U();\n", b)), "63\n");
    // Stdlib control: three singleton groups, one element each.
    BEZEL_EXPECT_EQ(run(mainOf("", "    var a: Array<int> := [1, 2, 3];\n",
                               std::string("    Console.WriteLine(a.GroupBy(") + kF +
                                   ").Select(lambda(g: IGrouping<int, int>) => g.Count()).Sum());\n")),
                    "3\n");
}

BEZEL_TEST(user_groupby_terminals_6711, where_count_spec_stdlib_5) {
    const std::string b = std::string("    Console.WriteLine(u.GroupBy(") + kF +
                          ").Where(lambda(x: int) => x > 10).Count());\n";
    BEZEL_EXPECT_EQ(run(mainOf(kUserCls, kU, b)), "2\n");
    BEZEL_EXPECT_EQ(run(mainOf(kExt, kU, b)), "2\n");
    BEZEL_EXPECT_EQ(run(mainOf(kIface, "    using var u: IU := new U();\n", b)), "2\n");
    BEZEL_EXPECT_EQ(run(mainOf("", "    var a: Array<int> := [1, 2, 3];\n",
                               std::string("    Console.WriteLine(a.GroupBy(") + kF +
                                   ").Where(lambda(g: IGrouping<int, int>) => g.Key > 1).Count());\n")),
                    "2\n");
}

BEZEL_TEST(user_groupby_terminals_6711, for_over_select_spec_stdlib_5) {
    const std::string b = std::string("    var s := 0;\n    for var v in u.GroupBy(") + kF +
                          ").Select(lambda(x: int) => x + 1) do s := s + v; end;\n"
                          "    Console.WriteLine(s);\n";
    BEZEL_EXPECT_EQ(run(mainOf(kUserCls, kU, b)), "63\n");
    BEZEL_EXPECT_EQ(run(mainOf(kExt, kU, b)), "63\n");
    BEZEL_EXPECT_EQ(run(mainOf(kIface, "    using var u: IU := new U();\n", b)), "63\n");
    BEZEL_EXPECT_EQ(run(mainOf("", "    var a: Array<int> := [1, 2, 3];\n",
                               std::string("    var s := 0;\n    for var v in a.GroupBy(") + kF +
                                   ").Select(lambda(g: IGrouping<int, int>) => g.Key) do s := s + v; end;\n"
                                   "    Console.WriteLine(s);\n")),
                    "6\n");
}
