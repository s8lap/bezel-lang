// Issue #6295 item 1 (spec §14.2, §27.6, v0.355; stdlib §2.2).
//
// `Math.Min/Max/Clamp<T>` are declared `where T: IComparable<T>` (stdlib §2.2), and
// §14.2 decides a TYPE-PARAMETER type argument by that parameter's OWN constraints:
//
//   §14.2 *A type parameter as the type argument*, case 1: "The argument is a type
//   parameter `P` ... the constraint is decided at that site, by `P`'s own
//   constraints. `P` satisfies the constraint exactly when its constraints guarantee
//   it at every substitution they admit, as a matter of language rule — an ordinary
//   interface's conforming set is open, so an interface bound excludes only what a
//   language rule excludes."
//
//   §14.2 *Interface conformance*, rule 6: "`T` is a type parameter whose constraints
//   guarantee conformance to `I`: a type-bound constraint naming `I` or an interface
//   extending `I`."
//
//   §14.2 *Conditionally-admitted members*, rule 2 states the shape outright:
//   "`Array<T>.Sort()` is rejected in a body whose `T` is unconstrained and admitted
//   under `where T: IComparable<T>`, as `x.CompareTo(y)` on a `T`-typed `x` would be."
//
// THE DEFECT. The formation check in the Math arm of `resolveMemberAccess` read the
// type argument as `ta->kind != Primitive` → refused. A type parameter's TypeRef is
// a declaration-less Named ref, so it took that arm and the VALID program
//
//     F<V>(): (V,V)->V where V: IComparable<V> => Math.Max<V>;
//
// was refused with "type argument does not satisfy the constraint `where T:
// IComparable<T>`" — while the callee form `Math.Max<V>(a,b)` in the same constrained
// body compiled and ran.
//
// THE FIX. The type-parameter arm asks the parameter's own constraints
// (`ExprTC::typeParamSatisfiesSelfComparable`, over the ONE constraint walk
// `forEachConstraintBound`). The RUNTIME half is the codegen thunk: a reference formed
// in a generic body names the ENCLOSING type parameter, and the thunk was built at
// the ERASED placeholder (the i32 width and the SIGNED comparison), so `F<uint>()`
// ordered `4000000000u` BELOW `3u` and `F<decimal>()` compared the decimal payload as
// a word. The thunk now resolves the parameter through the active monomorphization
// (`resolveTypeParamRef`), and the thunk CACHE KEY is computed from the SAME resolved
// type so the erased thunk and a specialization's thunk are distinct entries.
//
// The REJECT rows below are the anti-regression half: an unconstrained parameter and
// a parameter constrained only by `IEquatable<V>` must stay refused, and so must the
// nullable type argument (#6295 part 3) and the wrong type-argument count
// (#6295 part 2).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The constrained generic method whose body forms the reference.
const char* kConstrainedRef =
    "  public static method F<T>(): (T, T) -> T where T: IComparable<T> "
    "    return Math.Max<T>; "
    "  end F; ";

std::string programWithBody(const std::string& body) {
    return std::string("module Conf; public class Program ") + body +
           "  public static method Main() "
           "    Console.WriteLine(\"unused\"); "
           "  end Main; end Program;";
}

}  // namespace

// ── The rejected half ───────────────────────────────────────────────────────────

// An UNCONSTRAINED type parameter does not satisfy `where T: IComparable<T>`:
// §14.2 *Conditionally-admitted members* rule 2's own wording ("rejected in a body
// whose `T` is unconstrained").
BEZEL_TEST(sema_math_ref_constrained_typeparam_6295,
           unconstrained_type_parameter_is_refused_14_2) {
    auto r = runAot(programWithBody(
        "  public static method F<T>(): (T, T) -> T "
        "    return Math.Max<T>; "
        "  end F; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "IComparable<T>"));
}

// `where T: IEquatable<T>` does NOT establish `IComparable<T>` — the two are
// different interfaces (§14.2 rule 6 names the interface, not a family of them).
BEZEL_TEST(sema_math_ref_constrained_typeparam_6295,
           iequatable_only_type_parameter_is_refused_14_2) {
    auto r = runAot(programWithBody(
        "  public static method F<T>(): (T, T) -> T where T: IEquatable<T> "
        "    return Math.Max<T>; "
        "  end F; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "IComparable<T>"));
}

// #6295 part 3: a NULLABLE type argument never satisfies an interface constraint
// (§14.2) — it stays refused at the formation, at a real location.
BEZEL_TEST(sema_math_ref_constrained_typeparam_6295,
           nullable_type_argument_is_refused_14_2) {
    auto r = runAot(
        "module Conf; public class Program "
        "  public static method Main() "
        "    var m: (int?, int?) -> int? := Math.Max<int?>; "
        "  end Main; end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "IComparable<T>"));
}

// #6295 part 2: an explicit type-argument list must supply EXACTLY the declared
// count (§14.5) — the wrong count stays refused at a real location.
BEZEL_TEST(sema_math_ref_constrained_typeparam_6295,
           wrong_type_argument_count_is_refused_14_5) {
    auto r = runAot(
        "module Conf; public class Program "
        "  public static method Main() "
        "    var m: (int, int) -> int := Math.Max<int, int>; "
        "  end Main; end Program;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "type argument"));
}

// ── The admitted half ───────────────────────────────────────────────────────────

// The reference form under `where T: IComparable<T>`, at T = int. The callee form
// (`Math.Max<T>(a, b)`) already compiled and ran here; the REFERENCE form must
// agree with it (§27.6: the formation is a use of the member, read exactly as a
// call reads it).
BEZEL_TEST(sema_math_ref_constrained_typeparam_6295,
           constrained_reference_at_int_runs_27_6) {
    auto r = runAotChecked(
        std::string("module Conf; public class Program ") + kConstrainedRef +
        "  public static method Main() "
        "    var f: (int, int) -> int := F<int>(); "
        "    Console.WriteLine(f(3, 5)); "
        "    Console.WriteLine(f(9, 2)); "
        "  end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n9\n"));
}

// At T = string: stdlib §9.6 declares `public class String: ... IComparable<string>`,
// so `string` satisfies §2.2's `where T: IComparable<T>`, and the ordinal string
// ordering is the one `Math.Max` uses everywhere else.
BEZEL_TEST(sema_math_ref_constrained_typeparam_6295,
           constrained_reference_at_string_runs_27_6) {
    auto r = runAotChecked(
        std::string("module Conf; public class Program ") + kConstrainedRef +
        "  public static method Main() "
        "    var f: (string, string) -> string := F<string>(); "
        "    Console.WriteLine(f(\"abc\", \"abd\")); "
        "  end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("abd\n"));
}

// At T = a USER RECORD implementing `IComparable<Self>` (§14.2 rule 2 — a record
// that lists the interface in its inheritance clause and declares the matching
// member): the comparison must run through the record's own `CompareTo`.
BEZEL_TEST(sema_math_ref_constrained_typeparam_6295,
           constrained_reference_at_record_runs_27_6) {
    auto r = runAotChecked(
        "module Conf; "
        "record Pt(X: int, Y: int): IComparable<Pt> "
        "  public method CompareTo(other: Pt): int return X - other.X; end CompareTo; "
        "end Pt; "
        "public class Program "
        "  public static method F<T>(): (T, T) -> T where T: IComparable<T> "
        "    return Math.Max<T>; "
        "  end F; "
        "  public static method Main() "
        "    var f: (Pt, Pt) -> Pt := F<Pt>(); "
        "    var r := f(new Pt(1, 2), new Pt(3, 4)); "
        "    Console.WriteLine(r.X); "
        "    Console.WriteLine(r.Y); "
        "  end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n4\n"));
}

// The OTHER TWO selectors: `Min` and `Clamp` declare the same
// `where T: IComparable<T>` bound (the interface is stdlib §2.8) and are formed by the same check.
BEZEL_TEST(sema_math_ref_constrained_typeparam_6295,
           constrained_min_and_clamp_references_run_27_6) {
    auto r = runAotChecked(
        "module Conf; public class Program "
        "  public static method FMin<T>(): (T, T) -> T where T: IComparable<T> "
        "    return Math.Min<T>; "
        "  end FMin; "
        "  public static method FClamp<T>(): (T, T, T) -> T where T: IComparable<T> "
        "    return Math.Clamp<T>; "
        "  end FClamp; "
        "  public static method Main() "
        "    var g: (int, int) -> int := FMin<int>(); "
        "    var h: (int, int, int) -> int := FClamp<int>(); "
        "    Console.WriteLine(g(3, 5)); "
        "    Console.WriteLine(h(9, 1, 5)); "
        "  end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n5\n"));
}

// At T = `decimal`: stdlib §4.4 gives decimal a total order, so §2.2's
// `where T: IComparable<T>` admits it — but decimal lowers to a STRUCT, not to an
// LLVM integer, so the erased placeholder compared its PAYLOAD as a word (garbage).
BEZEL_TEST(sema_math_ref_constrained_typeparam_6295,
           constrained_reference_at_decimal_orders_by_decimal_27_6) {
    auto r = runAotChecked(
        std::string("module Conf; public class Program ") + kConstrainedRef +
        "  public static method Main() "
        "    var f: (decimal, decimal) -> decimal := F<decimal>(); "
        "    Console.WriteLine(f(1.5m, 2.25m)); "
        "  end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2.25\n"));
}

// At T = a CLASS implementing `IComparable<Self>`: the comparison must dispatch
// through the class's own `CompareTo` (§14.2 rule 1 — a class that lists the
// interface in its inheritance clause). At the erased placeholder the reference
// thunk was built for the placeholder's type, and the class's `CompareTo` was
// never reached (#7519 measured a SEGV here).
BEZEL_TEST(sema_math_ref_constrained_typeparam_6295,
           constrained_reference_at_class_runs_27_6) {
    auto r = runAotChecked(
        "module Conf; "
        "class Box: IComparable<Box> "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public method Value(): int => _v; "
        "  public method CompareTo(other: Box): int => _v - other.Value(); "
        "end Box; "
        "public class Program "
        "  public static method F<T>(): (T, T) -> T where T: IComparable<T> "
        "    return Math.Max<T>; "
        "  end F; "
        "  public static method Main() "
        "    var x := new Box(7); "
        "    var y := new Box(3); "
        "    var f: (Box, Box) -> Box := F<Box>(); "
        "    var r := f(x, y); "
        "    Console.WriteLine(r.Value()); "
        "  end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// §14.2 *Interface conformance* rule 6 admits a type parameter whose constraint
// names "an interface extending `I`": `where T: ICmp<T>` with
// `interface ICmp<T>: IComparable<T>` establishes the SAME guarantee as
// `where T: IComparable<T>`, because the bound's interface has `IComparable` in
// its extension chain (§14.2 rule 3 — "T is an interface that has I in its
// extension chain").
BEZEL_TEST(sema_math_ref_constrained_typeparam_6295,
           constrained_reference_over_an_extending_interface_runs_27_6) {
    auto r = runAotChecked(
        "module Conf; "
        "interface ICmp<T>: IComparable<T> "
        "end ICmp; "
        "record Pt(X: int): ICmp<Pt> "
        "  public method CompareTo(other: Pt): int return X - other.X; end CompareTo; "
        "end Pt; "
        "public class Program "
        "  public static method F<T>(): (T, T) -> T where T: ICmp<T> "
        "    return Math.Max<T>; "
        "  end F; "
        "  public static method Main() "
        "    var f: (Pt, Pt) -> Pt := F<Pt>(); "
        "    var r := f(new Pt(1), new Pt(3)); "
        "    Console.WriteLine(r.X); "
        "  end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

// The ORDERING the thunk emits must be T's OWN: stdlib §2.2 orders by T's
// `CompareTo`, so an UNSIGNED T compares unsigned (§14.5 / #6908's rule, applied
// to the reference form). At the erased placeholder the comparison was signed and
// `4000000000u` ranked BELOW `3u`.
BEZEL_TEST(sema_math_ref_constrained_typeparam_6295,
           constrained_reference_at_unsigned_orders_unsigned_27_6) {
    auto r = runAotChecked(
        std::string("module Conf; public class Program ") + kConstrainedRef +
        "  public static method Main() "
        "    var f: (uint, uint) -> uint := F<uint>(); "
        "    Console.WriteLine(f(3u, 4000000000u)); "
        "  end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4000000000\n"));
}
