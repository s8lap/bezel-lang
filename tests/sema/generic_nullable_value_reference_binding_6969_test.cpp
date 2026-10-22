// Regression tests for issue #6969 — spec §19.3 (v0.340), "Type parameters as
// pattern types", read with §8.6 (nullable value-type boxing) and §3.6 rule 2
// (per-instantiation evaluation).
//
// THE DEFECT. In a generic body `case v of var x: Object` over `v: T` is a
// reference-typed binding over a value. At an instantiation where `T` is a
// value type it is rejected AT THE INSTANTIATION SITE: §19.3 says "only a
// binding to a reference type, which would form the box, is rejected there".
// The instantiation check classified only NON-NULLABLE value substitutions as
// value types, so `G<int?>(42)` was admitted and printed `other` while
// `G<int>(42)` was rejected. A nullable value substitution forms the same box
// for its non-null values (§8.6: "If the value is non-null, the result is a
// boxed `T`"), so `T = int?` is rejected as `T = int` is.
//
// Reject rows assert a compile-time rejection with a real location, the rule's
// own wording, and the substituted type spelled as written. Accept rows are
// controls: the forms the fix must not reach keep the stdout they already
// produce. Value bindings over generic substitutions (`var p: Pt` at `T = Pt`,
// `var n: int` at `T = int?`) do not match where the spec says they should; that
// is a separate defect, filed rather than pinned here.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot, runAotChecked
#include "test_harness.hpp"

#include <string>

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A reject row: the program is refused at compile time with a real location,
// and the diagnostic carries the instantiation rule and the substitution.
bool rejectedAtNullableInstantiation(const std::string& program,
                                     const std::string& substitution) {
    auto r = runAot(program);
    if (!failedAtCompileTimeWithRealLocation(r)) return false;
    const std::string spelled = "at value substitution `" + substitution + "`";
    return diagnosticMessageContains(r, "rejected at the instantiation site") &&
           diagnosticMessageContains(r, spelled.c_str()) &&
           diagnosticMessageContains(r, "(\xC2\xA7""19.3, \xC2\xA7""3.6 rule 2)");
}

// An accept row: the program compiles, runs, and prints `expected`.
void expectRunsAndPrints(const JitResult& r, const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// The issue's literal repro (#6969): `G<int?>(42)` with `var x: Object` is
// rejected at the instantiation site, as `G<int>(42)` already is.
BEZEL_TEST(generic_nullable_value_reference_binding_6969,
           issue_repro_int_nullable_object_binding_rejected_spec_19_3) {
    BEZEL_EXPECT_TRUE(rejectedAtNullableInstantiation(
        R"BZ(
module M;
class C
  public static method G<T>(v: T): string
    return (case v of var x: Object => "bind", _ => "other");
  end G;
  public static method Main(): int
    Console.WriteLine(G<int?>(42));
    return 0;
  end Main;
end C;
)BZ",
        "int?"));
}

// The other nullable value substitutions: `long?`, `double?`, a record's `?`
// and an enum's `?`. Each program is refused with its own substitution named.
BEZEL_TEST(generic_nullable_value_reference_binding_6969,
           long_double_record_enum_nullable_rejected_spec_19_3) {
    BEZEL_EXPECT_TRUE(rejectedAtNullableInstantiation(
        R"BZ(
module M;
class C
  public static method G<T>(v: T): string
    return (case v of var x: Object => "bind", _ => "other");
  end G;
  public static method Main(): int
    Console.WriteLine(G<long?>(42));
    return 0;
  end Main;
end C;
)BZ",
        "long?"));
    BEZEL_EXPECT_TRUE(rejectedAtNullableInstantiation(
        R"BZ(
module M;
class C
  public static method G<T>(v: T): string
    return (case v of var x: Object => "bind", _ => "other");
  end G;
  public static method Main(): int
    Console.WriteLine(G<double?>(4.5));
    return 0;
  end Main;
end C;
)BZ",
        "double?"));
    BEZEL_EXPECT_TRUE(rejectedAtNullableInstantiation(
        R"BZ(
module M;
record Pt(X: int, Y: int);
class C
  public static method G<T>(v: T): string
    return (case v of var x: Object => "bind", _ => "other");
  end G;
  public static method Main(): int
    Console.WriteLine(G<Pt?>(new Pt(1, 2)));
    return 0;
  end Main;
end C;
)BZ",
        "Pt?"));
    BEZEL_EXPECT_TRUE(rejectedAtNullableInstantiation(
        R"BZ(
module M;
public enum Color Red, Green, Blue; end Color;
class C
  public static method G<T>(v: T): string
    return (case v of var x: Object => "bind", _ => "other");
  end G;
  public static method Main(): int
    Console.WriteLine(G<Color?>(Color.Red));
    return 0;
  end Main;
end C;
)BZ",
        "Color?"));
}

// A null argument at `T = int?` is rejected too: the rejection is decided by
// the substitution, not by the value the call passes.
BEZEL_TEST(generic_nullable_value_reference_binding_6969,
           null_argument_at_nullable_substitution_rejected_spec_19_3) {
    BEZEL_EXPECT_TRUE(rejectedAtNullableInstantiation(
        R"BZ(
module M;
class C
  public static method G<T>(v: T): string
    return (case v of var x: Object => "bind", _ => "other");
  end G;
  public static method Main(): int
    Console.WriteLine(G<int?>(null));
    return 0;
  end Main;
end C;
)BZ",
        "int?"));
}

// The case STATEMENT form: the same binding rule in the statement's arms.
BEZEL_TEST(generic_nullable_value_reference_binding_6969,
           statement_form_nullable_substitution_rejected_spec_19_3) {
    BEZEL_EXPECT_TRUE(rejectedAtNullableInstantiation(
        R"BZ(
module M;
class C
  public static method Q<T>(v: T): int
    var r: int := 0;
    case v of var x: Object => r := 1, _ => r := 0 end case;
    return r;
  end Q;
  public static method Main(): int
    Console.WriteLine(Q<int?>(42));
    return 0;
  end Main;
end C;
)BZ",
        "int?"));
}

// A type parameter of an ENCLOSING CLASS (`class Box<T>`), instantiated as
// `new Box<int?>()`, is decided at the same site (§3.6 rule 2).
BEZEL_TEST(generic_nullable_value_reference_binding_6969,
           class_level_parameter_nullable_substitution_rejected_spec_19_3) {
    BEZEL_EXPECT_TRUE(rejectedAtNullableInstantiation(
        R"BZ(
module M;
class Box<T>
  public constructor() end constructor;
  public method Show(v: T): string
    return (case v of var x: Object => "bind", _ => "other");
  end Show;
end Box;
class C
  public static method Main(): int
    var b := new Box<int?>();
    Console.WriteLine(b.Show(42));
    return 0;
  end Main;
end C;
)BZ",
        "int?"));
}

// The `v: T?` carrier at `T = int?`.
BEZEL_TEST(generic_nullable_value_reference_binding_6969,
           nullable_carrier_at_nullable_substitution_rejected_spec_19_3) {
    BEZEL_EXPECT_TRUE(rejectedAtNullableInstantiation(
        R"BZ(
module M;
class C
  public static method G<T>(v: T?): string
    return (case v of var x: Object => "bind", _ => "other");
  end G;
  public static method Main(): int
    Console.WriteLine(G<int?>(42));
    return 0;
  end Main;
end C;
)BZ",
        "int?"));
}

// Class and interface bindings are reference-typed too: `var d: D` and
// `var s: IShape` at `T = int?` are rejected for the same reason.
BEZEL_TEST(generic_nullable_value_reference_binding_6969,
           class_and_interface_bindings_at_int_nullable_rejected_spec_19_3) {
    BEZEL_EXPECT_TRUE(rejectedAtNullableInstantiation(
        R"BZ(
module M;
public class D end D;
class C
  public static method G<T>(v: T): string
    return (case v of var d: D => "d", _ => "other");
  end G;
  public static method Main(): int
    Console.WriteLine(G<int?>(42));
    return 0;
  end Main;
end C;
)BZ",
        "int?"));
    BEZEL_EXPECT_TRUE(rejectedAtNullableInstantiation(
        R"BZ(
module M;
public interface IShape method Id(): int; end IShape;
class C
  public static method G<T>(v: T): string
    return (case v of var s: IShape => "shape", _ => "other");
  end G;
  public static method Main(): int
    Console.WriteLine(G<int?>(42));
    return 0;
  end Main;
end C;
)BZ",
        "int?"));
}

// Nullable reference bindings are reference-typed bindings: `var x: Object?`,
// `var s: string?` and `var s: IShape?` at `T = int?` are rejected too.
BEZEL_TEST(generic_nullable_value_reference_binding_6969,
           nullable_reference_bindings_at_int_nullable_rejected_spec_19_3) {
    BEZEL_EXPECT_TRUE(rejectedAtNullableInstantiation(
        R"BZ(
module M;
class C
  public static method G<T>(v: T): string
    return (case v of var x: Object? => "bind", _ => "other");
  end G;
  public static method Main(): int
    Console.WriteLine(G<int?>(42));
    return 0;
  end Main;
end C;
)BZ",
        "int?"));
    BEZEL_EXPECT_TRUE(rejectedAtNullableInstantiation(
        R"BZ(
module M;
class C
  public static method G<T>(v: T): string
    return (case v of var s: string? => "s", _ => "other");
  end G;
  public static method Main(): int
    Console.WriteLine(G<int?>(42));
    return 0;
  end Main;
end C;
)BZ",
        "int?"));
    BEZEL_EXPECT_TRUE(rejectedAtNullableInstantiation(
        R"BZ(
module M;
public interface IShape method Id(): int; end IShape;
class C
  public static method G<T>(v: T): string
    return (case v of var s: IShape? => "shape", _ => "other");
  end G;
  public static method Main(): int
    Console.WriteLine(G<int?>(42));
    return 0;
  end Main;
end C;
)BZ",
        "int?"));
}

// Reference substitutions stay admitted with `var x: Object`: a string, a
// nullable string, a class and a nullable class. The class instance is bound to
// a local (a temporary passed straight to a parameter is a separate leak, #6975),
// and the checked oracle arms the sanitizers.
BEZEL_TEST(generic_nullable_value_reference_binding_6969,
           reference_substitutions_stay_accepted_spec_19_3) {
    auto r = runAotChecked(R"BZ(
module M;
public class D end D;
class C
  public static method G<T>(v: T): string
    return (case v of var x: Object => "bind", _ => "other");
  end G;
  public static method Main(): int
    var d: D := new D();
    Console.WriteLine(G<string>("s"));
    Console.WriteLine(G<string?>("s"));
    Console.WriteLine(G<string?>(null));
    Console.WriteLine(G<D>(d));
    Console.WriteLine(G<D?>(d));
    Console.WriteLine(G<D?>(null));
    return 0;
  end Main;
end C;
)BZ");
    expectRunsAndPrints(r, "bind\nbind\nother\nbind\nbind\nother\n");
}

// `where T: class` admits the class and string substitutions, and the
// reference-typed nullable substitutions (an interface `?`, `Object?`) keep
// their answers under `var x: Object` and `var s: IShape`.
BEZEL_TEST(generic_nullable_value_reference_binding_6969,
           where_class_and_nullable_reference_substitutions_stay_accepted_spec_19_3) {
    auto r = runAot(R"BZ(
module M;
public interface IShape method Id(): int; end IShape;
public class Dq : IShape public method Id(): int return 1; end Id; end Dq;
class C
  public static method H<T>(v: T): string where T: class
    return (case v of var x: Object => "bind", _ => "other");
  end H;
  public static method S<T>(v: T): string
    return (case v of var s: IShape => "shape", _ => "other");
  end S;
  public static method K<T>(v: T): string
    return (case v of var x: Object => "bind", _ => "other");
  end K;
  public static method Main(): int
    Console.WriteLine(H<string>("s"));
    Console.WriteLine(S<IShape?>(new Dq()));
    Console.WriteLine(S<IShape?>(null));
    Console.WriteLine(K<Object?>(42));
    Console.WriteLine(K<Object?>(null));
    return 0;
  end Main;
end C;
)BZ");
    expectRunsAndPrints(r, "bind\nshape\nother\nbind\nother\n");
}

// The forms the rule does not reach at a nullable value substitution keep their
// answers: a bare `Object` pattern (no binding; §19.5 never matches null), an
// untyped `var x`, the `is Object` test, and the statement form of the bare
// pattern (§19.3, "Type patterns without binding").
BEZEL_TEST(generic_nullable_value_reference_binding_6969,
           bare_object_untyped_and_is_forms_stay_accepted_spec_19_3) {
    auto r = runAot(R"BZ(
module M;
class C
  public static method G<T>(v: T): string
    return (case v of Object => "obj", _ => "other");
  end G;
  public static method U<T>(v: T): string
    return (case v of var x => "any", _ => "other");
  end U;
  public static method F<T>(v: T): bool
    return (v is Object);
  end F;
  public static method Q<T>(v: T): int
    var r: int := 0;
    case v of Object => r := 1, _ => r := 0 end case;
    return r;
  end Q;
  public static method Main(): int
    Console.WriteLine(G<int?>(5));
    Console.WriteLine(G<int?>(null));
    Console.WriteLine(U<int?>(42));
    Console.WriteLine(F<int?>(42));
    Console.WriteLine(F<int?>(null));
    Console.WriteLine(Q<int?>(42));
    Console.WriteLine(Q<int?>(null));
    return 0;
  end Main;
end C;
)BZ");
    expectRunsAndPrints(r, "obj\nother\nany\nTrue\nFalse\n1\n0\n");
}

// A value binding at a NON-nullable value substitution is admitted and matches:
// `var x: int` at `T = int` has identical types. (Control; the substitution is
// not nullable, so this row is not reached by the fix.)
BEZEL_TEST(generic_nullable_value_reference_binding_6969,
           value_binding_at_value_substitution_stays_accepted_spec_19_3) {
    auto r = runAot(R"BZ(
module M;
class C
  public static method G<T>(v: T): int
    return (case v of var x: int => x + 1, _ => -1);
  end G;
  public static method Main(): int
    Console.WriteLine(G<int>(42));
    return 0;
  end Main;
end C;
)BZ");
    expectRunsAndPrints(r, "43\n");
}

// `var x: int` at `T = int?` is ADMITTED: it is not a reference-typed binding,
// so the fix does not reject it. Its VALUE is not pinned here. Whether a nullable
// substitution matches an `int` pattern is an open reading of §19.3 (the
// "identical types" rule against its "(over a nullable scrutinee, its non-null
// values)" parenthetical), filed as a spec-question; the current runtime answer
// (-1 for 42) is a separate defect, filed, not pinned.
BEZEL_TEST(generic_nullable_value_reference_binding_6969,
           value_binding_at_nullable_substitution_admitted_spec_19_3) {
    auto r = runAot(R"BZ(
module M;
class C
  public static method G<T>(v: T): int
    return (case v of var x: int => x + 1, _ => -1);
  end G;
  public static method Main(): int
    Console.WriteLine(G<int?>(42));
    return 0;
  end Main;
end C;
)BZ");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
