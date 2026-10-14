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
//   R5  record receiver: captured BY COPY (§27.6 "by copy for value-shaped
//       (record, enum or primitive) receivers"), formed and invoked.
//   R6  a choice receiver has no generic thunk: located error, no miscompile.
//   R7  constraint matrix (§27.6 "the formation is an instantiation site":
//       §14.2's constraints read at the formation as at a call): a method with
//       a `where` clause is never accepted unchecked at a formation.
//   R8  overload selection with a list is independent of declaration order.
//   R9  a by-value `a: T` slot at a function-type argument is §28.14's
//       set-erasing formation ("a bare type parameter that the formation's
//       substitution makes a function type").
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

// R5 — record receiver by copy.
BEZEL_TEST(sema_generic_method_reference_list_5814,
           record_receiver_captured_by_copy_27_6) {
    auto r = runAotChecked(
        "module Conf; "
        "public record Pt(X: int, Y: int) "
        "  public method Sum<T>(z: int, t: T): int => this.X + this.Y + z; "
        "end Pt; "
        "public class Prog "
        "  public static method Run(f: (int, string) -> int): int => "
        "    f(1, \"a\") + f(2, \"b\"); "
        "  public static method Main() "
        "    var p := new Pt(3, 4); "
        "    Console.WriteLine(Run(p.Sum<string>)); "
        "    var p2 := new Pt(10, 10); "
        "    var q: (int, string) -> int := p2.Sum<string>; "
        "    Console.WriteLine(q(0, \"x\")); "
        "  end Main; end Prog;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("17\n20\n"));
}

// R6 — choice receiver.
BEZEL_TEST(sema_generic_method_reference_list_5814,
           choice_receiver_has_no_generic_thunk_located_error_27_6) {
    auto r = runAot(
        "module Conf; "
        "public choice Shape Circle(R: int), Dot; "
        "  public method Tag<T>(t: T): int => 1; "
        "end Shape; "
        "public class Prog public static method Main() "
        "  var s := Shape.Circle(3); "
        "  var f: (string) -> int := s.Tag<string>; "
        "end Main; end Prog;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a value-shaped receiver other than a record has no generic thunk"));
}

namespace {
std::string constrainedProg(const char* constraint, const char* usage) {
    return std::string("module Conf; public record NoEq(A: int); "
                       "public class Util "
                       "  public static method F<T>(a: T): int where T: ") +
           constraint + " return 5; end F; end Util; "
           "public class Prog public static method Main() " + usage +
           " end Main; end Prog;";
}
}  // namespace

// R7 — each constraint kind, non-conforming and conforming reference: never
// accepted unchecked at a formation (the call is rejected for the first of
// each pair by §14.2, the formation by the not-yet-supported error).
BEZEL_TEST(sema_generic_method_reference_list_5814,
           where_class_nonconforming_formation_rejected_14_2) {
    auto r = runAot(constrainedProg("class",
        "var f: (int) -> int := Util.F<int>;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not yet supported at a formation"));
}
BEZEL_TEST(sema_generic_method_reference_list_5814,
           where_value_nonconforming_formation_rejected_14_2) {
    auto r = runAot(constrainedProg("value",
        "var f: (string) -> int := Util.F<string>;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not yet supported at a formation"));
}
BEZEL_TEST(sema_generic_method_reference_list_5814,
           where_notnull_nonconforming_formation_rejected_14_2) {
    auto r = runAot(constrainedProg("notnull",
        "var f: (string?) -> int := Util.F<string?>;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not yet supported at a formation"));
}
BEZEL_TEST(sema_generic_method_reference_list_5814,
           where_new_nonconforming_formation_rejected_14_2) {
    auto r = runAot(constrainedProg("new()",
        "var f: (string) -> int := Util.F<string>;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not yet supported at a formation"));
}
BEZEL_TEST(sema_generic_method_reference_list_5814,
           where_idisposable_nonconforming_formation_rejected_14_2) {
    auto r = runAot(constrainedProg("IDisposable",
        "var f: (int) -> int := Util.F<int>;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not yet supported at a formation"));
}
BEZEL_TEST(sema_generic_method_reference_list_5814,
           where_class_conforming_formation_not_accepted_unchecked_14_2) {
    auto r = runAot(constrainedProg("class",
        "var f: (string) -> int := Util.F<string>;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not yet supported at a formation"));
}
// The call with the same list keeps its own §14.2 verdict.
BEZEL_TEST(sema_generic_method_reference_list_5814,
           where_class_call_with_same_list_still_rejected_14_2) {
    auto r = runAot(constrainedProg("class",
        "var r := Util.F<int>(1);"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "does not satisfy the constraint"));
}

// R8 — overload order independence (§27.6 / §14.5).
BEZEL_TEST(sema_generic_method_reference_list_5814,
           list_selects_generic_overload_declared_after_nongeneric_14_5) {
    auto r = runAotChecked(
        "module Conf; public class Util "
        "  public static method Over(x: int): int => x + 1; "
        "  public static method Over<T>(x: T): T => x; "
        "  public static method Id<T>(x: T): T => x; "
        "  public static method Id<A, B>(x: A, y: B): A => x; "
        "end Util; "
        "public class Prog "
        "  public static method Ap(f: (int) -> int): int => f(10); "
        "  public static method Main() "
        "    var b: (int) -> int := Util.Over<int>; "
        "    Console.WriteLine(b(1)); "
        "    Console.WriteLine(Ap(Util.Over<int>)); "
        "    var c: (int, string) -> int := Util.Id<int, string>; "
        "    Console.WriteLine(c(7, \"x\")); "
        "    var d: (int) -> int := Util.Id<int>; "
        "    Console.WriteLine(d(8)); "
        "  end Main; end Prog;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n10\n7\n8\n"));
}
BEZEL_TEST(sema_generic_method_reference_list_5814,
           list_selects_generic_overload_declared_before_nongeneric_14_5) {
    auto r = runAotChecked(
        "module Conf; public class Util "
        "  public static method Over<T>(x: T): T => x; "
        "  public static method Over(x: int): int => x + 1; "
        "end Util; "
        "public class Prog "
        "  public static method Ap(f: (int) -> int): int => f(10); "
        "  public static method Main() "
        "    Console.WriteLine(Ap(Util.Over<int>)); "
        "    var g: (int) -> int := Util.Over; "
        "    Console.WriteLine(g(1)); "
        "  end Main; end Prog;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10\n2\n"));
}

// R9 — by-value `a: T` at a function-type argument (§28.14 formation bar).
BEZEL_TEST(sema_generic_method_reference_list_5814,
           by_value_type_param_slot_at_function_type_is_set_erasing_28_14) {
    auto r = runAot(
        "module Conf; public class Util "
        "  public static method Take<T>(a: T): int => 1; end Util; "
        "public class Prog public static method Main() "
        "  var f: (() -> int) -> int := Util.Take<() -> int>; "
        "end Main; end Prog;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "a set-erasing position"));
}

// s27_6_0013 — `Math.Max` has only generic overloads, so a reference without
// its type-argument list is rejected (it compiled and the binary crashed,
// rc 139, before the fix).
BEZEL_TEST(sema_generic_method_reference_list_5814,
           bare_math_max_reference_rejected_not_crashing_27_6) {
    auto r = runAot(
        "module Conf; "
        "public class Program public static method Main() "
        "  var m: (int, int) -> int := Math.Max; "
        "  Console.WriteLine(m(3, 5)); "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Math.Max` is a reference to a generic method and names no type arguments"));
}

// s27_6_0014 — `Math.Max<int>` formed with its list is a function value
// `(int, int) -> int` (§27.6, v0.338). Before the fix the reference was left
// untyped, codegen's CG60 produced a null closure cell and the invocation
// crashed (rc 139). Min/Clamp/long/double/string/record arguments are read,
// and the formation is looped under the ASan oracle for the cell's release.
BEZEL_TEST(sema_generic_method_reference_list_5814,
           math_max_with_type_argument_list_is_formed_and_invoked_27_6) {
    auto r = runAotChecked(
        "module Conf; "
        "public record Pt: IComparable<Pt> "
        "  public x: int; "
        "  public method CompareTo(o: Pt): int => this.x - o.x; "
        "end Pt; "
        "public class Program "
        "  public static method Apply(f: (int, int) -> int, a: int, b: int): int "
        "    => f(a, b); "
        "  public static method Main() "
        "    var m: (int, int) -> int := Math.Max<int>; "
        "    Console.WriteLine(m(3, 5)); "
        "    var d: (double, double) -> double := Math.Min<double>; "
        "    Console.WriteLine(d(3.5, 1.25)); "
        "    var c: (int, int, int) -> int := Math.Clamp<int>; "
        "    Console.WriteLine(c(9, 1, 5)); "
        "    var s: (string, string) -> string := Math.Max<string>; "
        "    Console.WriteLine(s(\"a\", \"b\")); "
        "    var p: (Pt, Pt) -> Pt := Math.Max<Pt>; "
        "    Console.WriteLine(p(new Pt(1), new Pt(4)).x); "
        "    var t := 0; var i := 0; "
        "    while i < 200 do "
        "      var f: (int, int) -> int := Math.Max<int>; "
        "      t := t + f(i, 3) + Apply(Math.Min<int>, i, 7); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(t); "
        "    Console.WriteLine(Math.Max<int>(1, 2)); "
        "  end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n1.25\n5\nb\n4\n21278\n2\n"));
}

// §27.6 / §14.2 (v0.338): the formation is an instantiation site, so
// `Math.Max<W>` over a type with no `IComparable<W>` is rejected there, as the
// call `Math.Max<W>(a, b)` is.
BEZEL_TEST(sema_generic_method_reference_list_5814,
           math_max_reference_constraint_checked_at_formation_27_6) {
    auto r = runAot(
        "module Conf; "
        "public class W public constructor() end constructor; end W; "
        "public class Program public static method Main() "
        "  var m: (W, W) -> W := Math.Max<W>; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "IComparable<T>"));
}
