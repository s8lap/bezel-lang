// Spec-derived regression tests for issue #7789: the `?.` spelling
// of the §26.5 rule 1 commitment for a built-in collection receiver.
//
// Spec §26.5 rule 1 (v0.350): "If `x`'s declared type (or any base type)
// *declares* any instance member named `Foo` — of any arity, applicability,
// or accessibility — instance resolution is committed and extension blocks
// are **never consulted** for that name on that receiver. If the committed
// instance lookup then fails ..., the call is a compile error citing the
// instance member(s); it does not fall back to an extension."
// Stdlib §4.4 (v0.350) declares `Array<T>.Contains(item: T): bool`.
//
// #7700 gated the PLAIN `.` spelling for a concrete built-in collection head.
// The `?.` call is resolved by a separate function (resolveNullCondMember),
// which still consulted the user's extension blocks:
//
//   extension Array<int> public method Contains(x: int): bool => false; end;
//   var n: Array<int>? := a;
//   n?.Contains(2)   // #7789: printed False; the instance member says True
//
// The ORACLES are the exact stdout of the value rows and the specific
// diagnostic of the rejection rows. The CONTROLS are load-bearing: a name the
// head does not declare still binds the extension (rule 2).
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

std::string prog(const std::string& ext, const std::string& body) {
    return "module M;\nimport System.Linq;\n" + ext +
           "class C\n"
           "  public static method Main(): int\n"
           "    var a: Array<int> := [1,2,3];\n"
           "    var n: Array<int>? := a;\n"
           "    var z: Array<int>? := null;\n"
           "    var l: IList<int>? := a;\n"
           "    var st: Set<int> := [1,2,3];\n"
           "    var sn: Set<int>? := st;\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

void expectRejected(const std::string& ext, const std::string& body,
                    const std::string& needle) {
    auto r = runAot(prog(ext, body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

void expectStdout(const std::string& ext, const std::string& body,
                  const std::string& out) {
    auto r = runAot(prog(ext, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// ── #7789: the instance member wins for `?.` ──────────────────────────────

// The issue's repro (conformance s26_5_1_0035): the user `Contains` returns
// false, the declared one answers True.
BEZEL_TEST(sema_builtin_nullcond_collection_rule1_7789,
           array_nullcond_contains_binds_the_instance_member) {
    // per §26.5 rule 1 (v0.350) + stdlib §4.4
    expectStdout("extension Array<int>\n"
                 "  public method Contains(x: int): bool => false;\n"
                 "end;\n",
                 "Console.WriteLine(n?.Contains(2) ?? false);",
                 "True\n");
}

// A null receiver short-circuits: the extension is not reached either.
BEZEL_TEST(sema_builtin_nullcond_collection_rule1_7789,
           array_nullcond_contains_null_receiver_is_null) {
    // per §7.5 + §26.5 rule 1 (v0.350)
    expectStdout("extension Array<int>\n"
                 "  public method Contains(x: int): bool => true;\n"
                 "end;\n",
                 "Console.WriteLine(z?.Contains(2) ?? false);",
                 "False\n");
}

// The extension typed the call as `string`; the declared member types it
// `bool?`, so a `string` sink is refused with §5.1's diagnostic.
BEZEL_TEST(sema_builtin_nullcond_collection_rule1_7789,
           array_nullcond_contains_string_extension_does_not_type_the_call) {
    // per §26.5 rule 1 (v0.350) + stdlib §4.4 + §5.1
    expectRejected("extension Array<int>\n"
                   "  public method Contains(x: int): string => \"EXT\";\n"
                   "end;\n",
                   "var s: string? := n?.Contains(2);\n"
                   "    Console.WriteLine(s);",
                   "not implicitly convertible to the target type");
}

// "Of any arity": the extension's `IndexOf` takes two arguments, the
// declared member `IndexOf(item, startIndex := 0)` one; the name commits.
BEZEL_TEST(sema_builtin_nullcond_collection_rule1_7789,
           array_nullcond_indexof_other_arity_extension_unreachable) {
    // per §26.5 rule 1 (v0.350) + stdlib §4.4
    expectStdout("extension Array<int>\n"
                 "  public method IndexOf(x: int, y: int): string => \"EXT\";\n"
                 "end;\n",
                 "Console.WriteLine(n?.IndexOf(2) ?? -9);",
                 "1\n");
}

// Another concrete head: `Set<T>` conforms to `ICollection<T>` (§4.10).
BEZEL_TEST(sema_builtin_nullcond_collection_rule1_7789,
           set_nullcond_contains_binds_the_instance_member) {
    // per §26.5 rule 1 (v0.350) + stdlib §4.1, §4.10
    expectStdout("extension Set<int>\n"
                 "  public method Contains(x: int): bool => false;\n"
                 "end;\n",
                 "Console.WriteLine(sn?.Contains(2) ?? false);",
                 "True\n");
}

// CONTROL: `Frobnicate` is declared by no collection head, so the user's
// extension still binds under `?.` (§26.5 rule 2).
BEZEL_TEST(sema_builtin_nullcond_collection_rule1_7789,
           control_nullcond_undeclared_name_binds_the_extension) {
    // per §26.5 rule 2 + stdlib §4.4
    expectStdout("extension Array<int>\n"
                 "  public method Frobnicate(x: int): int => x + 40;\n"
                 "end;\n",
                 "Console.WriteLine(n?.Frobnicate(2) ?? -9);",
                 "42\n");
}

// CONTROL: stdlib §5's `Sum` is not a member of `Array<T>`; the user's more
// specific block wins under `?.` (§26.5 rules 2-3), as it does under `.`.
BEZEL_TEST(sema_builtin_nullcond_collection_rule1_7789,
           control_nullcond_linq_name_binds_the_user_extension) {
    // per §26.5 rules 2-3 + stdlib §4.4, §5
    expectStdout("extension Array<int>\n"
                 "  public method Sum(): int => 77;\n"
                 "end;\n",
                 "Console.WriteLine(n?.Sum() ?? -1);",
                 "77\n");
}
