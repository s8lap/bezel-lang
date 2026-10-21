// Spec-derived regression test for issue #6709, the cross-module leg (see
// tests/sema/overload_derived_user_generic_iface_6709_test.cpp for the
// single-module rows and the root cause).
//
// Spec (docs/Bezel_Language_Spec.md, v0.339):
//   §22.6: a candidate is applicable when each argument converts to its
//     parameter.
//   §12.9: "Type parameters without `out` or `in` are invariant."; "under
//     `out`, `I<S>` converts to `I<T>` when `S` so converts to `T`".
//
// Imported `IBox<T>` / `ICov<out T>` / `Box<T>` / `DogBox: Box<Dog>` and an
// imported overload pair for each, called with the imported `DogBox` and an
// app-local `Pup: DogBox`. FAIL-WITHOUT (MEASURED): "ambiguous call to `F`".
//
// The app declares no overload set of its own: an app-local overload set
// declared before the imported call shifts LLVM's name uniquing and the call
// fails to link, on staging too (#6256).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::ModuleSource;
using bezel::test::runAotModules;

BEZEL_TEST(xmod_overload_derived_user_generic_iface_6709,
           imported_derived_class_pair_spec_22_6_12_9) {
    const char* lib = R"bzl(module L;
public class Animal public constructor() end; end Animal;
public class Dog: Animal public constructor() end; end Dog;
public interface IBox<T> method Get(): int; end IBox;
public interface ICov<out T> method Get(): int; end ICov;
public class Box<T>: IBox<T>, ICov<T>
  public constructor() end;
  public method Get(): int => 0;
end Box;
public class DogBox: Box<Dog> public constructor() end; end DogBox;
public static class S
  public static method F(x: IBox<string>): int => 2;
  public static method F(x: IBox<Dog>): int => 1;
  public static method G(x: ICov<Animal>): int => 3;
  public static method G(x: ICov<string>): int => 4;
end S;)bzl";
    const char* app = R"bzl(module A;
import L;
class Pup: DogBox public constructor() end; end Pup;
class P public static method Main(): int
  using var a := new DogBox();
  using var p := new Pup();
  Console.WriteLine(S.F(a));
  Console.WriteLine(S.G(a));
  Console.WriteLine(S.F(p));
  Console.WriteLine(S.G(p));
  return 0; end Main; end P;)bzl";
    auto r = runAotModules({ModuleSource{"L", lib}, ModuleSource{"A", app}},
                           "A");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n3\n1\n3\n"));
}
