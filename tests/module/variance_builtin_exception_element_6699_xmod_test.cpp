// Spec-derived regression test for issue #6699, the cross-module leg (see
// tests/sema/variance_builtin_exception_element_6699_test.cpp for the
// single-module rows and the root cause).
//
// Spec (docs/Bezel_Language_Spec.md, v0.339):
//   §12.9: "under `out`, `I<S>` converts to `I<T>` when `S` so converts to
//     `T`".
//   §5.1 (Reference conversions): "**class to base class**, transitively, up
//     to `Object`".
//
// An IMPORTED `MyErr: ArgumentException` reaches `Exception` only through its
// declared built-in base, which an imported type carries off the wire
// (`nonSymbolBaseRefsOf`, #3139). FAIL-WITHOUT (MEASURED): rejected with "the
// initializer for `e` has a type that is not implicitly convertible to the
// target type (§5.1)".

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::ModuleSource;
using bezel::test::runAotModules;

BEZEL_TEST(xmod_variance_builtin_exception_element_6699,
           imported_exception_subclass_elements_spec_12_9_5_1) {
    const char* lib = R"bzl(module L;
public class MyErr: ArgumentException
  public constructor(m: string) base(m); end;
end MyErr;
public static class S
  public static method Count(xs: IEnumerable<Exception>): int
    var n := 0;
    for var v in xs do Console.WriteLine(v.Message); n := n + 1; end;
    return n;
  end Count;
end S;)bzl";
    const char* app = R"bzl(module A;
import L;
class P public static method Main(): int
  using var x := new MyErr("xm");
  using var a := new Array<MyErr>();
  a.Add(x);
  var e: IReadOnlyList<Exception> := a;
  Console.WriteLine(e[0].Message);
  Console.WriteLine(S.Count(a));
  return 0; end Main; end P;)bzl";
    auto r = runAotModules({ModuleSource{"L", lib}, ModuleSource{"A", app}},
                           "A");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("xm\nxm\n1\n"));
}
