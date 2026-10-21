// Spec-derived regression tests for issue #6227, the cross-module leg of the
// fix-forward (see tests/sema/overload_builtin_generic_argument_resolve_6227_
// test.cpp for the single-module rows and the root cause).
//
// Spec (docs/Bezel_Language_Spec.md, v0.339):
//   §22.6: "two candidates that both require an implicit conversion at the
//     same position are **equally ranked** there"; with one candidate
//     applicable it is selected.
//   §12.9: "For value-typed T, the only legal assignment of `IEnumerable<T>` is
//     to `IEnumerable<T>` itself or to `Object`."
//
// An imported `S.G(IEnumerable<int>)` / `S.G(IEnumerable<string>)` pair: an
// `Array<int>` argument reaches `IEnumerable` only as `IEnumerable<int>`.
// FAIL-WITHOUT (MEASURED at the rebased 3a4c8faf): `app2` was rejected with
// "the call to `G`: argument 1 has a type that is not implicitly convertible
// to the target type (§5.1)". `app4` is the #6227 fix's own cross-module
// shape (an imported `Array<Exception>` / `Array<ArgumentException>` pair on a
// static and a virtual member), KEPT.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::ModuleSource;
using bezel::test::runAotModules;

namespace {

const char* kLib = R"bzl(module L;
public static class S
  public static method F(x: Array<Exception>): int => 1;
  public static method F(x: Array<ArgumentException>): int => 2;
  public static method G(x: IEnumerable<int>): int => 3;
  public static method G(x: IEnumerable<string>): int => 4;
end S;
public class B
  public constructor() end;
  public virtual method F(x: Array<Exception>): int => 1;
  public virtual method F(x: Array<ArgumentException>): int => 2;
end B;)bzl";

void expectRunsXmod(const char* app, const char* out) {
    auto r = runAotModules({ModuleSource{"L", kLib}, ModuleSource{"A", app}},
                           "A");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

}  // namespace

BEZEL_TEST(xmod_overload_builtin_generic_argument_resolve_6227,
           imported_builtin_interface_pair_spec_22_6_12_9) {
    expectRunsXmod(R"bzl(module A;
import L;
class P public static method Main(): int
  using var i := new Array<int>();
  using var s := new Array<string>();
  Console.WriteLine(S.G(i));
  Console.WriteLine(S.G(s));
  return 0; end Main; end P;)bzl",
                   "3\n4\n");
}

BEZEL_TEST(xmod_overload_builtin_generic_argument_resolve_6227,
           imported_builtin_array_pair_kept_spec_22_6_12_9) {
    expectRunsXmod(R"bzl(module A;
import L;
class P public static method Main(): int
  using var a := new Array<ArgumentException>();
  using var e := new Array<Exception>();
  Console.WriteLine(S.F(a));
  Console.WriteLine(S.F(e));
  using var b := new B();
  Console.WriteLine(b.F(a));
  Console.WriteLine(b.F(e));
  return 0; end Main; end P;)bzl",
                   "2\n1\n2\n1\n");
}
