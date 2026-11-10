// Regression tests for the R41 CONSTRAINT FAMILY lane — issues #8240, #8262
// and #8312 — one root cause seen from four sides:
//
//   "When the compiler checks a built-in or stdlib member's conditionally-admitted
//    member condition (§14.2 rule 6), it looks at the type ARGUMENT's structure
//    — does the type name `IComparable`, does it have a `CompareTo` — but it
//    does NOT consult the enclosing method's declared type-parameter
//    CONSTRAINTS."
//
// THE SPEC AUTHORITY (v0.355).
//
//   * spec §14.2 *Interface conformance* rule 6 — "T is a type parameter whose
//     constraints guarantee conformance to I: a type-bound constraint naming I
//     or an interface extending I …".
//   * spec §14.2 *Conditionally-admitted members* — "In a generic body, where
//     the receiver's type arguments, or those of a result instantiation the
//     condition reads, are or contain a type parameter in scope, the condition
//     is read as the section defining it reads a type parameter, under that
//     parameter's constraints."
//   * spec §3.6 rule 2 — "A generic body type-checks under the weakest
//     assumption its constraints establish."
//   * stdlib §5.1 — `public extension IEnumerable<T> where T: IComparable<T>`
//     with `public method Min(): T; public method Max(): T;`.
//   * stdlib §4.4 — `public method Sort() where T: IComparable<T>;` and
//     `public method Sorted(): owned Array<T> where T: IComparable<T>;`.
//   * stdlib §2.2 — `public static method Min<T>(a: T, b: T): T
//     where T: IComparable<T>;` (and `Max` / `Clamp`).
//
// THE THREE ISSUES.
//
//   * #8240 — the no-selector `Min()` / `Max()` over an `Array<T>` in a body
//     whose `T` is bounded `where T: IComparable<T>` was REFUSED (codegen's
//     #2770 backstop: "`T` does not — its `CompareTo` could not be resolved
//     for dispatch"), although the bound GUARANTEES the ordering.
//   * #8262 — `Array<T>.Sort()` / `Array<T>.Sorted()` (stdlib §4.4's
//     conditionally-admitted members) were REFUSED for every type parameter.
//   * #8312 — a generic `Math.Max(a, b)` forwarding under
//     `where T: IComparable<T>` was REFUSED by codegen's #2786 diagnostic.
//
// THE ORDER ORACLE. The rows below that assert an ORDER use an element class
// whose `CompareTo` is REVERSED with respect to the value order, and an array
// whose order differs from BOTH, so the three candidate answers differ:
//
//     value order  : min = 1, max = 3
//     array order  : min = a[0] = 3, max = a[2] = 2
//     CompareTo    : K(3) < K(2) < K(1)  ->  min = 3, max = 1
//
// A row printing 3 and 1 therefore proves the order came from the element's own
// `CompareTo` — not from the values, and not from the array's layout.
//
// THE CONTROLS are the second half of the file: every one of them is a shape
// the fix must NOT admit. They are MEASURED rows, not assertions about the
// code.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::JitResult;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The rejection oracle: the named diagnostic, at a real location, and the
// compiler did not merely abort.
void expectRefused(const JitResult& r, const std::string& needle) {
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            found = true;
    BEZEL_EXPECT_TRUE(found);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// The program's stdout, verbatim. "<did-not-run>" when the program was
// rejected, crashed or timed out.
std::string outChecked(const std::string& program) {
    auto r = runAotChecked(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}

std::string lines(std::initializer_list<std::string> parts) {
    std::string s;
    for (const auto& p : parts) {
        s += p;
        s += "\n";
    }
    return s;
}

// The element class. `CompareTo` is REVERSED with respect to the value order
// (see THE ORDER ORACLE above), so every order-asserting row can tell the
// three candidate answers apart.
std::string progK(const std::string& body) {
    return R"(
module M;
import System.Collections.Generic;
public class K: IComparable<K>
  public X: int;
  public constructor(x: int) X := x; end;
  public method CompareTo(o: K): int => o.X - X;
end K;
public class G
  public static method Lo<T>(xs: Array<T>): T where T: IComparable<T> => xs.Min();
  public static method Hi<T>(xs: Array<T>): T where T: IComparable<T> => xs.Max();
  public static method Lazy<T>(xs: Array<T>): T where T: IComparable<T> => xs.Where(lambda(x: T) => true).Max();
end G;
public class C
  public static method Main(): int
    var a: Array<K> := [new K(3), new K(1), new K(2)];
)" + body + R"(
    return 0;
  end Main;
end C;
)";
}

}  // namespace

// ---------------------------------------------------------------------------
// #8240 — the no-selector `Min()` / `Max()` over `Array<T>` in a generic body
// whose `T` is bounded `where T: IComparable<T>`.
// ---------------------------------------------------------------------------

// stdlib §5.1: `Min(): T` over `where T: IComparable<T>`. The element's own
// `CompareTo` orders it — REVERSED, so the answer is 3 (the value order would
// say 1, and the array order would say 3 too — the two are told apart by the
// `Max()` twin below).
BEZEL_TEST(constraint_family_8240_8262_8312,
           min_over_a_type_parameter_guaranteeing_icomparable_is_admitted) {
    BEZEL_EXPECT_EQ(outChecked(progK("    Console.WriteLine(G.Lo<K>(a).X);")),
                    lines({"3"}));
}

// The `Max()` twin: the reversed `CompareTo` makes K(1) the largest, so the
// answer is 1. A fix that compared the VALUES would print 3; a fix that read
// the ARRAY's layout would print 2.
BEZEL_TEST(constraint_family_8240_8262_8312,
           max_over_a_type_parameter_guaranteeing_icomparable_is_admitted) {
    BEZEL_EXPECT_EQ(outChecked(progK("    Console.WriteLine(G.Hi<K>(a).X);")),
                    lines({"1"}));
}

// The LAZY twin — `xs.Where(…).Max()` — is the §5.1 extension over the lazy
// sequence, and the brief names it explicitly. It must be admitted too, and
// it must order by the element's own `CompareTo` (1, not 3).
BEZEL_TEST(constraint_family_8240_8262_8312,
           lazy_max_over_a_type_parameter_guaranteeing_icomparable_is_admitted) {
    BEZEL_EXPECT_EQ(outChecked(progK("    Console.WriteLine(G.Lazy<K>(a).X);")),
                    lines({"1"}));
}

// The §29.2 WRAPPER twin — the same §5.1 terminal over an `IEnumerable<T>`
// receiver (the callee's formal), where the receiver reaches the body through
// the §29.11 iterator-VTABLE wrapper rather than as a `bz_array_t*`. The
// wrapper's element type is the SOURCE's (the array actually walked), and the
// §5.1 terminal that drains it reads the CONCRETE element: the two must agree,
// or the comparator is handed half a pointer. (This is the shape that
// SEGFAULTED at the tip before the wrapper's element type was repaired.)
BEZEL_TEST(constraint_family_8240_8262_8312,
           min_over_an_ienumerable_type_parameter_is_admitted) {
    BEZEL_EXPECT_EQ(
        outChecked(R"(
module M;
import System.Collections.Generic;
public class K: IComparable<K>
  public X: int;
  public constructor(x: int) X := x; end;
  public method CompareTo(o: K): int => o.X - X;
end K;
public class G
  public static method Lo<T>(xs: IEnumerable<T>): T where T: IComparable<T> => xs.Min();
end G;
public class C
  public static method Main(): int
    var a: Array<K> := [new K(3), new K(1), new K(2)];
    Console.WriteLine(G.Lo<K>(a).X);
    return 0;
  end Main;
end C;
)"),
        lines({"3"}));
}

// The LAZY twin of the same — `xs.Where(…).Max()` over an `IEnumerable<T>`
// receiver. The lazy source is the materialized snapshot of the wrapper, so
// its stride is the CONCRETE element's too.
//
// The element's `CompareTo` is REVERSED (see THE ORDER ORACLE above), so the
// largest element is K(1) — the answer is `1`, not the value order's `3`.
BEZEL_TEST(constraint_family_8240_8262_8312,
           lazy_max_over_an_ienumerable_type_parameter_is_admitted) {
    BEZEL_EXPECT_EQ(
        outChecked(R"(
module M;
import System.Collections.Generic;
public class K: IComparable<K>
  public X: int;
  public constructor(x: int) X := x; end;
  public method CompareTo(o: K): int => o.X - X;
end K;
public class G
  public static method Lazy<T>(xs: IEnumerable<T>): T where T: IComparable<T> => xs.Where(lambda(x: T) => true).Max();
end G;
public class C
  public static method Main(): int
    var a: Array<K> := [new K(3), new K(1), new K(2)];
    Console.WriteLine(G.Lazy<K>(a).X);
    return 0;
  end Main;
end C;
)"),
        lines({"1"}));
}

// ---------------------------------------------------------------------------
// #8262 — `Array<T>.Sort()` / `Array<T>.Sorted()` (stdlib §4.4's
// conditionally-admitted members).
// ---------------------------------------------------------------------------

// `public method Sort() where T: IComparable<T>;` — in place, and the order
// it imposes is the element's own `CompareTo` (K(3), K(2), K(1)).
BEZEL_TEST(constraint_family_8240_8262_8312,
           sort_over_a_type_parameter_guaranteeing_icomparable_is_admitted) {
    BEZEL_EXPECT_EQ(
        outChecked(R"(
module M;
import System.Collections.Generic;
public class K: IComparable<K>
  public X: int;
  public constructor(x: int) X := x; end;
  public method CompareTo(o: K): int => o.X - X;
end K;
class S
  public static method Go<T>(a: Array<T>): owned Array<T> where T: IComparable<T>
    a.Sort();
    var b := new Array<T>();
    for var x in a do b.Add(x); end;
    return b;
  end Go;
end S;
class C
  public static method Main(): int
    var a: Array<K> := [new K(3), new K(1), new K(2)];
    using var b := S.Go<K>(a);
    Console.WriteLine(b[0].X);
    Console.WriteLine(b[1].X);
    Console.WriteLine(b[2].X);
    return 0;
  end Main;
end C;
)"),
        lines({"3", "2", "1"}));
}

// `public method Sorted(): owned Array<T> where T: IComparable<T>;` — the COPY
// form. The brief asks for it to be MEASURED and REPORTED: it is the same
// gate, and it was refused on the base exactly as `Sort()` was.
BEZEL_TEST(constraint_family_8240_8262_8312,
           sorted_over_a_type_parameter_guaranteeing_icomparable_is_admitted) {
    BEZEL_EXPECT_EQ(
        outChecked(R"(
module M;
import System.Collections.Generic;
public class K: IComparable<K>
  public X: int;
  public constructor(x: int) X := x; end;
  public method CompareTo(o: K): int => o.X - X;
end K;
class S
  public static method Go<T>(a: Array<T>): owned Array<T> where T: IComparable<T>
    return a.Sorted();
  end Go;
end S;
class C
  public static method Main(): int
    var a: Array<K> := [new K(3), new K(1), new K(2)];
    using var b := S.Go<K>(a);
    Console.WriteLine(b[0].X);
    Console.WriteLine(b[1].X);
    Console.WriteLine(b[2].X);
    return 0;
  end Main;
end C;
)"),
        lines({"3", "2", "1"}));
}

// ---------------------------------------------------------------------------
// #8312 — a generic `Math.Max(a, b)` forwarding under `where T:
// IComparable<T>`.
// ---------------------------------------------------------------------------

// stdlib §2.2: `public static method Max<T>(a: T, b: T): T where T:
// IComparable<T>;`. The generic body forwards to it, and the ORDERING the
// constraint guarantees is the one the comparison must use. At `T = int` the
// answer is 4; at `T = string` (§9.6's ordinal relation) it is `y`.
BEZEL_TEST(constraint_family_8240_8262_8312,
           math_max_over_a_type_parameter_guaranteeing_icomparable_is_admitted) {
    BEZEL_EXPECT_EQ(
        outChecked(R"(
module M;
public static class G
  public static method F<T>(a: T, b: T): T where T: IComparable<T> => Math.Max(a, b);
end G;
class C
  public static method Main(): int
    Console.WriteLine(G.F(3, 4));
    Console.WriteLine(G.F("x", "y"));
    return 0;
  end Main;
end C;
)"),
        lines({"4", "y"}));
}

// ---------------------------------------------------------------------------
// THE CONTROLS — the shapes the fix must NOT admit.
// ---------------------------------------------------------------------------

// CONTROL 1 — an UNCONSTRAINED type parameter guarantees nothing (spec §14.2
// rule 6), so the body cannot assume the element is comparable (spec §3.6
// rule 2). The call stays a compile error.
BEZEL_TEST(constraint_family_8240_8262_8312,
           min_over_an_unconstrained_type_parameter_is_refused) {
    auto r = runAot(R"(
module M;
import System.Collections.Generic;
class G
  public static method F<T>(xs: Array<T>): T => xs.Min();
end G;
class C
  public static method Main(): int
    var a: Array<int> := [3, 1, 2];
    Console.WriteLine(G.F<int>(a));
    return 0;
  end Main;
end C;
)");
    expectRefused(r, "does not GUARANTEE `IComparable<T>`");
}

// CONTROL 2 — `T: IComparable<int>` names the WRONG argument. `IComparable`
// is INVARIANT (spec §12.9), so `T : IComparable<int>` does NOT establish
// `T : IComparable<T>` — the constraint must name the parameter itself.
BEZEL_TEST(constraint_family_8240_8262_8312,
           min_over_a_type_parameter_bound_to_the_wrong_argument_is_refused) {
    auto r = runAot(R"(
module M;
import System.Collections.Generic;
class G
  public static method F<T>(xs: Array<T>): T where T: IComparable<int> => xs.Min();
end G;
class C
  public static method Main(): int
    var a: Array<int> := [3, 1, 2];
    Console.WriteLine(G.F<int>(a));
    return 0;
  end Main;
end C;
)");
    expectRefused(r, "does not GUARANTEE `IComparable<T>`");
}

// CONTROL 3 — an UNCONSTRAINED CLASS element that does not implement
// `IComparable` must still be refused. This is the §5.1 gate's own message
// (#1198), and it must be unchanged.
BEZEL_TEST(constraint_family_8240_8262_8312,
           min_over_a_class_that_does_not_implement_icomparable_is_refused) {
    auto r = runAot(R"(
module M;
import System.Collections.Generic;
public class P
  public X: int;
  public constructor(x: int) X := x; end;
end P;
class C
  public static method Main(): int
    var a: Array<P> := [new P(3), new P(1)];
    Console.WriteLine(a.Min().X);
    return 0;
  end Main;
end C;
)");
    expectRefused(r, "does not implement IComparable<T>");
}

// CONTROL 4 — `Array<Plain>.Sort()` (a class that does not implement
// `IComparable<Plain>`) must still be refused, with the §4.4 gate's own
// wording.
BEZEL_TEST(constraint_family_8240_8262_8312,
           sort_over_a_class_that_does_not_implement_icomparable_is_refused) {
    auto r = runAot(R"(
module M;
import System.Collections.Generic;
public class Plain
  public X: int;
  public constructor(x: int) X := x; end;
end Plain;
class C
  public static method Main(): int
    var a: Array<Plain> := [new Plain(3), new Plain(1)];
    a.Sort();
    return 0;
  end Main;
end C;
)");
    expectRefused(r, "requires T: IComparable<T>");
}

// CONTROL 5 — `Array<T>.Sort()` with an UNCONSTRAINED `T` stays a compile
// error (the §4.4 gate).
BEZEL_TEST(constraint_family_8240_8262_8312,
           sort_over_an_unconstrained_type_parameter_is_refused) {
    auto r = runAot(R"(
module M;
import System.Collections.Generic;
class S
  public static method Go<T>(a: Array<T>): int
    a.Sort();
    return a.Count;
  end Go;
end S;
class C
  public static method Main(): int
    var a: Array<int> := [3, 1, 2];
    Console.WriteLine(S.Go<int>(a));
    return 0;
  end Main;
end C;
)");
    expectRefused(r, "requires T: IComparable<T>");
}

// CONTROL 6 — `Math.Max(a, b)` in a NON-GENERIC body is unchanged.
BEZEL_TEST(constraint_family_8240_8262_8312,
           math_max_in_a_non_generic_body_is_unchanged) {
    BEZEL_EXPECT_EQ(
        outChecked(R"(
module M;
class C
  public static method Main(): int
    Console.WriteLine(Math.Max(3, 4));
    Console.WriteLine(Math.Max("x", "y"));
    Console.WriteLine(Math.Min(3, 4));
    return 0;
  end Main;
end C;
)"),
        lines({"4", "y", "3"}));
}

// CONTROL 7 — a generic `Math.Max(a, b)` whose `T` is NOT bounded by
// `IComparable<T>` stays a compile error (stdlib §2.2's condition).
BEZEL_TEST(constraint_family_8240_8262_8312,
           math_max_over_an_unconstrained_type_parameter_is_refused) {
    auto r = runAot(R"(
module M;
public static class G
  public static method F<T>(a: T, b: T): T => Math.Max(a, b);
end G;
class C
  public static method Main(): int
    Console.WriteLine(G.F(3, 4));
    return 0;
  end Main;
end C;
)");
    expectRefused(r, "does not satisfy the constraint `where T: IComparable<T>`");
}

// CONTROL 8 — the `Guid` half of the §4.4 Sort gate (#8237) must not
// regress: `Guid: IComparable<Guid>` (stdlib §2.6), and §2.6's order is over
// the 16 CANONICAL bytes.
BEZEL_TEST(constraint_family_8240_8262_8312,
           guid_sort_is_unchanged) {
    BEZEL_EXPECT_EQ(
        outChecked(R"(
module M;
import System.Collections.Generic;
class C
  public static method Main(): int
    var a: Array<Guid> := [Guid.Parse("22222222-2222-2222-2222-222222222222"), Guid.Parse("11111111-1111-1111-1111-111111111111"), Guid.Parse("33333333-3333-3333-3333-333333333333")];
    a.Sort();
    for var x in a do Console.WriteLine(x.ToString()); end;
    return 0;
  end Main;
end C;
)"),
        lines({"11111111-1111-1111-1111-111111111111",
               "22222222-2222-2222-2222-222222222222",
               "33333333-3333-3333-3333-333333333333"}));
}

// CONTROL 9 — the `userRecordSelfComparable` path (a user record with its
// own `CompareTo`) is unchanged: `Min()` / `Max()` over `Array<R>` still
// answer 1 and 3.
BEZEL_TEST(constraint_family_8240_8262_8312,
           user_record_min_max_is_unchanged) {
    BEZEL_EXPECT_EQ(
        outChecked(R"(
module M;
import System.Collections.Generic;
public record R: IComparable<R>
  public X: int;
  public constructor(x: int) X := x; end;
  public method CompareTo(o: R): int => X - o.X;
end R;
class C
  public static method Main(): int
    var a: Array<R> := [new R(3), new R(1), new R(2)];
    Console.WriteLine(a.Min().X);
    Console.WriteLine(a.Max().X);
    return 0;
  end Main;
end C;
)"),
        lines({"1", "3"}));
}
