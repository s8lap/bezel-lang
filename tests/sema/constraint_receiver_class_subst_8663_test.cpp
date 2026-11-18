// fix #8663 (spec §14.1 *Instantiation and its site* and §14.2
// *Constraints*, v0.357): a generic METHOD's `where` clause belongs to the
// DECLARING TYPE's member set, so a bound that names the ENCLOSING CLASS's
// type parameter is read at the call under the RECEIVER's class
// instantiation.
//
// SPEC BASIS (QUOTED, v0.357 — spec §14.1, "Instantiation and its site").
//   "Wherever a rule of this specification fires *at the instantiation site* —
//    §3.6 rule 2's per-instantiation property evaluation, this section's
//    constraint checks (§14.2), ... — it fires at every such site."
// SPEC BASIS (QUOTED, v0.357 — spec §14.2, "Type-parameter bounds").
//   "A constraint whose bound is another type parameter — of the same
//    declaration, or of the enclosing type — is grammatical and has subtype
//    semantics: a substitution S for `T` satisfies `where T: U` exactly when S
//    is non-nullable and is a *subtype of* the substitution for `U`".
//
// BEFORE THE FIX: the bound was read with the class's own parameter
// UNSUBSTITUTED (a bare marker), and the conformance walk read that marker as
// "the constraint's own parameter" (§14.2's single-entry substitution) — so
// `d.Z<Num2>(...)` on a `D<Num>` receiver was compared against `IBox<Num2>`
// and ADMITTED. The body then read the `Num2` as a `Num`: the program
// compiles, runs, and prints `0`.
//
// Every row below FAILS without the fix. The must-still-admit shapes (the
// conforming argument, the receiver whose class argument is a type parameter
// in scope) are base-green controls and live in the conformance corpus, not
// here.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

// ---------------------------------------------------------------------------
// R1 — THE ISSUE'S REPRO (verbatim): a generic METHOD's `where S: IBox<Y>`
// bound naming the enclosing CLASS's type parameter, called on a `D<Num>`
// receiver with `S := Num2` (`Num2` is `IBox<Num2>`, NOT `IBox<Num>`).
// Before the fix: compiles, runs, prints `0` (a `Num2` read as a `Num`).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_receiver_class_subst_8663,
           issue_repro_receiver_class_argument_substitutes_the_bound) {
    JitResult r = runAot(R"(
module M;
interface IBox<T>
  static method Make(): owned T;
end IBox;
class Num: IBox<Num>
  public readonly property V: int;
  public constructor(v: int)
    V := v;
  end constructor;
  public static method Make(): owned Num => new Num(3);
end Num;
class Num2: IBox<Num2>
  public readonly property W: string;
  public constructor(w: string)
    W := w;
  end constructor;
  public static method Make(): owned Num2 => new Num2("four");
end Num2;
class D<Y>
  public constructor()
  end constructor;
  public method Z<S>(a: S): owned Y where S: IBox<Y> => S.Make();
end D;
class P
  public static method Main(): int
    var d := new D<Num>();
    using var n := d.Z<Num2>(new Num2("a"));   // S := Num2, Num2 is IBox<Num2>, NOT IBox<Num>
    Console.WriteLine(n.V);
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `Num2` does not satisfy the constraint `where S: "
           "IBox` (\xC2\xA7" "14.2)"));
}

// ---------------------------------------------------------------------------
// R2 — the same defect through INFERRED type arguments (§14.5: the
// constraint is checked after inference completes, on the full
// substitution): `d.Z(new Num2("a"))` binds `S := Num2` the same way.
// Before the fix: compiles, runs, prints `0`.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_receiver_class_subst_8663,
           inferred_type_argument_is_checked_under_the_receivers_class_argument) {
    JitResult r = runAot(R"(
module M;
interface IBox<T>
  static method Make(): owned T;
end IBox;
class Num: IBox<Num>
  public readonly property V: int;
  public constructor(v: int)
    V := v;
  end constructor;
  public static method Make(): owned Num => new Num(3);
end Num;
class Num2: IBox<Num2>
  public readonly property W: string;
  public constructor(w: string)
    W := w;
  end constructor;
  public static method Make(): owned Num2 => new Num2("four");
end Num2;
class D<Y>
  public constructor()
  end constructor;
  public method Z<S>(a: S): owned Y where S: IBox<Y> => S.Make();
end D;
class P
  public static method Main(): int
    var d := new D<Num>();
    using var n := d.Z(new Num2("a"));
    Console.WriteLine(n.V);
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `Num2` does not satisfy the constraint `where S: "
           "IBox`"));
}

// ---------------------------------------------------------------------------
// R3 — the INHERITED member reached through a DERIVED receiver: the class
// instantiation `D<Num>` is fixed by the derived class's base clause, so the
// bound is read under it (`IBox<Y>` at a `D<Num>` receiver is `IBox<Num>`).
// Before the fix: compiles, runs, prints `0`.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_receiver_class_subst_8663,
           derived_receiver_fixes_the_class_instantiation_the_bound_is_read_under) {
    JitResult r = runAot(R"(
module M;
interface IBox<T>
  static method Make(): owned T;
end IBox;
class Num: IBox<Num>
  public readonly property V: int;
  public constructor(v: int)
    V := v;
  end constructor;
  public static method Make(): owned Num => new Num(3);
end Num;
class Num2: IBox<Num2>
  public readonly property W: string;
  public constructor(w: string)
    W := w;
  end constructor;
  public static method Make(): owned Num2 => new Num2("four");
end Num2;
class D<Y>
  public constructor()
  end constructor;
  public method Z<S>(a: S): owned Y where S: IBox<Y> => S.Make();
end D;
class E: D<Num>
  public constructor()
  end constructor;
end E;
class P
  public static method Main(): int
    var e := new E();
    using var n := e.Z<Num2>(new Num2("a"));
    Console.WriteLine(n.V);
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `Num2` does not satisfy the constraint `where S: "
           "IBox`"));
}

// ---------------------------------------------------------------------------
// R4 — the bound naming the enclosing CLASS's type parameter DIRECTLY
// (`where S: Y`): §14.2's type-parameter-bounds paragraph names "the
// enclosing type" explicitly, so `S` must be a subtype of the receiver's
// class argument (`Num`), and `Num2` is not. Before the fix: the bound was
// never checked at the call (the class's parameter is not one of the
// method's own), so the call was admitted and the program printed `0`.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_receiver_class_subst_8663,
           bound_naming_the_enclosing_class_parameter_rejects_a_non_subtype) {
    JitResult r = runAot(R"(
module M;
class Num
  public readonly property V: int;
  public constructor(v: int)
    V := v;
  end constructor;
end Num;
class Num2
  public readonly property W: string;
  public constructor(w: string)
    W := w;
  end constructor;
end Num2;
class D<Y>
  public constructor()
  end constructor;
  public method Z<S>(a: S): owned Y where S: Y => a;
end D;
class P
  public static method Main(): int
    var d := new D<Num>();
    using var n := d.Z<Num2>(new Num2("a"));
    Console.WriteLine(n.V);
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `Num2` does not satisfy the constraint `where S: "
           "Num`"));
}

// ---------------------------------------------------------------------------
// R5 — the QUALIFIED static spelling of the same call (`D<Num>.Z<Num2>(…)`):
// §14.1 names "a generic type named in **receiver position** of a static
// member access" an instantiation site, so `D<Num>` fixes the class's type
// argument there too and the bound is `IBox<Num>`. Before the fix: admitted
// (the bound was read with `Y` unsubstituted) and the program printed `0`.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_receiver_class_subst_8663,
           qualified_static_spelling_fixes_the_class_instantiation_too) {
    JitResult r = runAot(R"(
module M;
interface IBox<T>
  static method Make(): owned T;
end IBox;
class Num: IBox<Num>
  public readonly property V: int;
  public constructor(v: int)
    V := v;
  end constructor;
  public static method Make(): owned Num => new Num(3);
end Num;
class Num2: IBox<Num2>
  public readonly property W: string;
  public constructor(w: string)
    W := w;
  end constructor;
  public static method Make(): owned Num2 => new Num2("four");
end Num2;
class D<Y>
  public constructor()
  end constructor;
  public static method Z<S>(a: S): owned Y where S: IBox<Y> => S.Make();
end D;
class P
  public static method Main(): int
    using var n := D<Num>.Z<Num2>(new Num2("a"));
    Console.WriteLine(n.V);
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `Num2` does not satisfy the constraint `where S: "
           "IBox`"));
}
