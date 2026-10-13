// #5814 (spec §27.6 "References to generic methods", §14.5, §28.14, v0.338;
// proposal 0164): a reference to a generic method names its type arguments on
// the member name (`Util.Id<int>`), all or none, and is formed at that
// substitution; no inference binds them from the target. Before the fix the
// parser's §22.12 scan admitted `>` only before `(`, so every reference with a
// list was a parse error, and a bare reference was a generic
// "not implicitly convertible".
//
// Rows (conformance twins: s27_6_0009, s27_6_0010, s27_6_0014, s28_14_0025):
//   R1  `Util.Id<int>` / `Id<string>` / `Wrap<long>` / `Pair2<int,string>` /
//       instance `u.Twice<int>` are formed and invoked (run, ASan oracle).
//   R2  `Util.Id` with no list over a generic-only method is rejected with the
//       list-required wording.
//   R3  a list of the wrong length is rejected.
//   R4  `b.Put<() -> int>` over `Put<T>(ref x: T)` is the §28.14 set-erasing
//       formation and is rejected (it parsed as an error before the fix, so
//       the reject must survive the parser change).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
const char* kPrelude =
    "module Conf; "
    "public class Box<T> "
    "  public constructor(v: T) this.V := v; end constructor; "
    "  public readonly V: T; "
    "end Box; "
    "public class Util "
    "  public static method Id<T>(x: T): T => x; "
    "  public static method Wrap<T>(x: T): owned Box<T> => new Box<T>(x); "
    "  public static method Pair2<A, B>(a: A, b: B): string => \"x\"; "
    "  public method Twice<T>(x: T, f: (T) -> T): T => f(f(x)); "
    "  public method Put<T>(ref x: T) end Put; "
    "end Util; ";
}  // namespace

BEZEL_TEST(sema_generic_method_reference_list_5814,
           reference_with_type_argument_list_is_formed_and_invoked_27_6) {
    auto r = runAotChecked(std::string(kPrelude) +
        "public class Program public static method Main() "
        "  var i: (int) -> int := Util.Id<int>; "
        "  var s: (string) -> string := Util.Id<string>; "
        "  Console.WriteLine(i(3)); "
        "  Console.WriteLine(s(\"hi\")); "
        "  var w: (long) -> owned Box<long> := Util.Wrap<long>; "
        "  using var b := w(7L); "
        "  Console.WriteLine(b.V); "
        "  var p: (int, string) -> string := Util.Pair2<int, string>; "
        "  Console.WriteLine(p(1, \"a\")); "
        "  var u := new Util(); "
        "  var t: (int, (int) -> int) -> int := u.Twice<int>; "
        "  Console.WriteLine(t(1, lambda(x) => x + 5)); "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nhi\n7\nx\n11\n"));
}

BEZEL_TEST(sema_generic_method_reference_list_5814,
           bare_reference_to_generic_method_names_no_type_arguments_27_6) {
    auto r = runAot(std::string(kPrelude) +
        "public class Program public static method Main() "
        "  var i: (int) -> int := Util.Id; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "is a reference to a generic method and names no type arguments"));
}

BEZEL_TEST(sema_generic_method_reference_list_5814,
           wrong_length_type_argument_list_rejected_27_6) {
    auto r = runAot(std::string(kPrelude) +
        "public class Program public static method Main() "
        "  var i: (int) -> int := Util.Id<int, int>; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "names 2 type argument(s), but the method declares 1"));
}

BEZEL_TEST(sema_generic_method_reference_list_5814,
           ref_param_at_function_type_formation_is_set_erasing_28_14) {
    auto r = runAot(std::string(kPrelude) +
        "public class Program public static method Main() "
        "  using var b := new Util(); "
        "  var put: (ref () -> int) -> () := b.Put<() -> int>; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "a set-erasing position"));
}
