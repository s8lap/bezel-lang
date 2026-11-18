// fix #8450 — a collection literal passed to the `IEnumerable<T>` parameter of
// the stdlib §5 LINQ operators `Concat` / `Zip` / `Union` / `Intersect` /
// `Except` was ACCEPTED and the query local degraded to a zero value
// (spec §15.4, v0.357; stdlib §5).
//
// WHAT THE DOCUMENTS REQUIRE.
//   spec §15.4 (v0.357): "A collection literal has no standalone type. It is
//   well-typed only in a context that provides the target type", and the
//   CLOSED list of "target types that admit collection literals" names NO
//   interface; and: "A collection literal in a position where no target type
//   is determinable is a compile error".
//   stdlib §5 (`System.Collections.Linq`):
//     public method Concat(other: IEnumerable<T>): IEnumerable<T>;
//     public method Zip<U, R>(other: IEnumerable<U>, combine: (T, U) -> R): IEnumerable<R>;
//     public method Union(other: IEnumerable<T>): IEnumerable<T>;
//     public method Intersect(other: IEnumerable<T>): IEnumerable<T>;
//     public method Except(other: IEnumerable<T>): IEnumerable<T>;
//   — the `other` parameter of every one of them is an INTERFACE, so a
//   collection literal there has no determinable target type.
//
// THE DEFECT (the issue's literal repro, measured on the base of this
// branch): the call compiled with NO diagnostic — only the #2434 warning
// "lowerLocalDecl's initializer expression failed to lower to a value for
// this local's slot (degraded to a zero-initialized value)" — so the query
// local `q` held a zero value. The oracle is therefore the DIAGNOSTIC (a
// silent acceptance is invisible in an exit code), and each row below pins
// the exact §15.4 refusal at the literal.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

std::string prog(const std::string& body) {
    return "module P;\npublic class Program\n"
           "  public static method Main()\n" + body +
           "\n  end Main;\nend Program;\n";
}

// The #1174 §15.4 refusal, naming the interface the parameter declares.
bool interfaceTargetRefused(const JitResult& r, const std::string& iface) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("cannot target the interface type `" + iface +
                           "`") != std::string::npos &&
            d.message.find("15.4") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// The issue's literal repro: `a.Concat([2, 3])` over an `Array<int>`.
BEZEL_TEST(sema_linq_ienumerable_literal_8450,
           concat_other_literal_rejected_15_4) {
    auto r = runAot(prog(
        "    var a: Array<int> := [1];\n"
        "    var q := a.Concat([2, 3]);\n"
        "    Console.WriteLine(q.Count());"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(interfaceTargetRefused(r, "IEnumerable<int>"));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// `Zip<U,R>(other: IEnumerable<U>, combine: (T, U) -> R)` — the `other`
// argument is the FIRST one; the second is the combine lambda.
BEZEL_TEST(sema_linq_ienumerable_literal_8450,
           zip_other_literal_rejected_15_4) {
    auto r = runAot(prog(
        "    var a: Array<int> := [1];\n"
        "    var q := a.Zip([2, 3], lambda(x: int, y: int) => x + y);\n"
        "    Console.WriteLine(q.Count());"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(interfaceTargetRefused(r, "IEnumerable<int>"));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// `Union(other: IEnumerable<T>)` — the `where T: hashable` block of stdlib
// §5, so the refusal must not be masked by the constraint gate.
BEZEL_TEST(sema_linq_ienumerable_literal_8450,
           union_other_literal_rejected_15_4) {
    auto r = runAot(prog(
        "    var a: Array<int> := [1];\n"
        "    var q := a.Union([2, 3]);\n"
        "    Console.WriteLine(q.Count());"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(interfaceTargetRefused(r, "IEnumerable<int>"));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// `Intersect(other: IEnumerable<T>)`.
BEZEL_TEST(sema_linq_ienumerable_literal_8450,
           intersect_other_literal_rejected_15_4) {
    auto r = runAot(prog(
        "    var a: Array<int> := [1];\n"
        "    var q := a.Intersect([2, 3]);\n"
        "    Console.WriteLine(q.Count());"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(interfaceTargetRefused(r, "IEnumerable<int>"));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// `Except(other: IEnumerable<T>)`.
BEZEL_TEST(sema_linq_ienumerable_literal_8450,
           except_other_literal_rejected_15_4) {
    auto r = runAot(prog(
        "    var a: Array<int> := [1];\n"
        "    var q := a.Except([2, 3]);\n"
        "    Console.WriteLine(q.Count());"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(interfaceTargetRefused(r, "IEnumerable<int>"));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// The same refusal on a CHAINED §5 receiver (`a.Where(..)` is the lazy
// sentinel the §5 typing mints), which is the other receiver shape the
// `other` parameter of these operators is reached through.
BEZEL_TEST(sema_linq_ienumerable_literal_8450,
           chained_lazy_receiver_literal_rejected_15_4) {
    auto r = runAot(prog(
        "    var a: Array<int> := [1];\n"
        "    var q := a.Where(lambda(x: int) => x > 0).Concat([2, 3]);\n"
        "    Console.WriteLine(q.Count());"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(interfaceTargetRefused(r, "IEnumerable<int>"));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}
