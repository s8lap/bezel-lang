// fix #8649 (spec §14.2 *Constraints on overrides and interface
// implementations* and §14.1 *Instantiation and its site*, v0.357): the
// INHERITED constraint set of an `override` or an interface implementation
// that OMITS its `where` clause is the method's own constraint set, so it is
// enforced at every instantiation of that method — exactly as the same set
// spelled out in a written clause is.
//
// SPEC BASIS (QUOTED, v0.357 — spec §14.2, "Constraints on overrides and
// interface implementations").
//   "An `override` of a generic method, and a method implementing a generic
//   interface method (an interface member declaring type parameters of its own,
//   whose `where` clauses sit where a class method's do: Appendix A.4
//   `IfaceMethod`, `IfaceStaticMethod`), inherits the overridden or implemented
//   declaration's constraints. Omitting the `where` clauses is the recommended
//   spelling and means exactly the inherited set".
//   "The two declarations' method-level type parameters, equal in number,
//   correspond by position (§11.2's *Generic members*), and the inherited set
//   is read under the implemented interface's or overridden base's
//   substitution, its method type parameters renamed positionally".
//
// BEFORE THE FIX: the instantiation-site check read the AST-WRITTEN clauses
// only, so with the clause omitted it found no constraint and admitted every
// type argument. With #8618 (the body's §11.8 view of the inherited set) and
// without this fix, such a program COMPILES and dispatches a static member the
// type argument does not have: it prints an empty line or garbage, or
// segfaults (the IBox and generic-class-face rows).
//
// Every row below FAILS without the fix (on #8618's tip c17a75ebe) except the
// one boundary control at the end, which is named with the wrong fix it
// catches.

#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;
using bezel::test::runAotModules;

// ---------------------------------------------------------------------------
// R1 — THE ISSUE'S REPRO (verbatim): `class D: B<int>` overrides
// `Z<S>(a: S): S where S: INum<S>` omitting the clause; `S := string` does not
// satisfy `INum<string>`. Before the fix: accepted, prints `hello`.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_inherited_set_enforced_8649,
           issue_repro_override_omitting_where_rejects_a_violating_argument) {
    JitResult r = runAot(R"(
module M;
interface INum<T>
  method Zero(): T;
end INum;
class B<X>
  public virtual method Z<S>(a: S): S where S: INum<S> => a;
end B;
class D: B<int>
  public override method Z<S>(a: S): S => a;
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z("hello"));
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `string` does not satisfy the constraint `where S: "
           "INum` (\xC2\xA7" "14.2)"));
}

// ---------------------------------------------------------------------------
// R2 — the class override whose body USES the inherited bound (`T.Zero()`,
// admitted by #8618). Before the fix: compiles, prints an empty line
// (`INumber<string>.Zero()` dispatched on `string`).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_inherited_set_enforced_8649,
           class_override_static_call_body_rejects_a_violating_argument) {
    JitResult r = runAot(R"(
module M;
class B
  public virtual method Z<T>(a: T): T where T: INumber<T> => T.Zero();
end B;
class D: B
  public override method Z<T>(a: T): T => T.Zero();
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z("x"));
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `string` does not satisfy the constraint `where T: "
           "INumber`"));
}

// ---------------------------------------------------------------------------
// R3 — the interface IMPLEMENTATION spelling of R2. Before the fix: compiles,
// prints an empty line.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_inherited_set_enforced_8649,
           interface_implementation_omitting_where_rejects_a_violating_argument) {
    JitResult r = runAot(R"(
module M;
interface I
  method Z<T>(a: T): T where T: INumber<T>;
end I;
class D: I
  public method Z<T>(a: T): T => T.Zero();
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z("x"));
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `string` does not satisfy the constraint `where T: "
           "INumber`"));
}

// ---------------------------------------------------------------------------
// R4 — the inherited set read under the implemented interface's substitution
// (`class D: IBox<Num>` reads `where S: IBox<T>` as `IBox<Num>`); `Num` does
// not implement `IBox<Num>`. Before the fix: compiles and SEGFAULTS (exit 139:
// the static `Make` is dispatched on `Num`).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_inherited_set_enforced_8649,
           interface_substitution_inherited_bound_rejects_a_nonconforming_argument) {
    JitResult r = runAot(R"(
module M;
interface IBox<T>
  method Z<S>(a: S): owned T where S: IBox<T>;
  static method Make(): owned T;
end IBox;
class Num
  public readonly property V: int;
  public constructor(v: int)
    V := v;
  end constructor;
end Num;
class D: IBox<Num>
  public method Z<S>(a: S): owned Num => S.Make();
  public static method Make(): owned Num => new Num(3);
end D;
class P
  public static method Main(): int
    var d := new D();
    using var n := d.Z<Num>(new Num(5));
    Console.WriteLine(n.V);
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `Num` does not satisfy the constraint `where S: "
           "IBox`"));
}

// ---------------------------------------------------------------------------
// R5 — the GENERIC CLASS FACE `class D<Y>: B<Y>`: the inherited set is read
// under the overridden base's substitution (`X := Y`). `Other` implements no
// `IBox` at all. Before the fix: compiles and SEGFAULTS (exit 139).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_inherited_set_enforced_8649,
           generic_class_face_override_rejects_a_nonconforming_argument) {
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
class Other
  public constructor()
  end constructor;
end Other;
class B<X>
  public virtual method Z<S>(a: S): owned X where S: IBox<X> => S.Make();
end B;
class D<Y>: B<Y>
  public constructor()
  end constructor;
  public override method Z<S>(a: S): owned Y => S.Make();
end D;
class P
  public static method Main(): int
    var d := new D<Num>();
    using var n := d.Z<Other>(new Other());
    Console.WriteLine(n.V);
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `Other` does not satisfy the constraint `where S: "
           "IBox`"));
}

// ---------------------------------------------------------------------------
// R6 — a CHAINED omission across two generic levels (`D: B<Num>`,
// `B<W>: A<W>`, only `A` writes the clause): the inherited set is climbed and
// read under the composed substitution. The conforming `Num` call stays
// admitted (no diagnostic names `Num`); the `Other` call is rejected. Before
// the fix: both calls compile and the program prints `3` twice.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_inherited_set_enforced_8649,
           chained_omission_rejects_only_the_nonconforming_argument) {
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
class Other
  public constructor()
  end constructor;
end Other;
class A<X>
  public virtual method Z<S>(a: S): int where S: IBox<X> => 1;
end A;
class B<W>: A<W>
  public override method Z<S>(a: S): int => 2;
end B;
class D: B<Num>
  public constructor()
  end constructor;
  public override method Z<S>(a: S): int => 3;
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z<Num>(new Num(5)));
    Console.WriteLine(d.Z<Other>(new Other()));
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `Other` does not satisfy the constraint `where S: "
           "IBox`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "type argument `Num`"));
}

// ---------------------------------------------------------------------------
// R7 — "exactly the inherited set" includes its KIND constraints, not only its
// interface bounds: `where T: class` omitted, `Z(5)`. Before the fix:
// accepted, prints `2`.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_inherited_set_enforced_8649,
           inherited_kind_constraint_rejects_a_value_type_argument) {
    JitResult r = runAot(R"(
module M;
class B
  public virtual method Z<T>(a: T): int where T: class => 1;
end B;
class D: B
  public constructor()
  end constructor;
  public override method Z<T>(a: T): int => 2;
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z(5));
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `int` does not satisfy the constraint `where T: "
           "class`"));
}

// ---------------------------------------------------------------------------
// R8 — an interface STATIC generic method (§14.2 names `IfaceStaticMethod`):
// its implementation omitting `where` inherits `where T: IMaker`, so
// `D.Z<string>()` is rejected. Before the fix: accepted, prints `5`.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_inherited_set_enforced_8649,
           interface_static_generic_implementation_rejects_a_violating_argument) {
    JitResult r = runAot(R"(
module M;
interface IMaker
  static method Make(): int;
end IMaker;
interface I
  static method Z<T>(): int where T: IMaker;
end I;
class D: I
  public static method Z<T>(): int => 5;
end D;
class P
  public static method Main(): int
    Console.WriteLine(D.Z<string>());
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `string` does not satisfy the constraint `where T: "
           "IMaker`"));
}

// ---------------------------------------------------------------------------
// R9 — the same interface STATIC generic method's body sees the inherited
// bound (`T.Make()`), the twin #8618 left out: the static conformance check
// recorded no witness, so the inherited-set lookup found nothing. Before the
// fix: "type parameter `T` has no static member `Make`" (§11.8).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_inherited_set_enforced_8649,
           interface_static_generic_implementation_body_sees_the_inherited_bound) {
    JitResult r = runAot(R"(
module M;
interface IMaker
  static method Make(): int;
end IMaker;
class K: IMaker
  public static method Make(): int => 42;
end K;
interface I
  static method Z<T>(): int where T: IMaker;
end I;
class D: I
  public static method Z<T>(): int => T.Make();
end D;
class P
  public static method Main(): int
    Console.WriteLine(D.Z<K>());
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// ---------------------------------------------------------------------------
// R10 — an OPERATOR (`a + a`) in an override that omits `where` resolves
// through the inherited `INumber<T>` bound (§11.8 / §25.2: a type parameter's
// operators come from its constraints, and the inherited set IS its
// constraints). Before the fix: "operator `+` is not available on type
// parameter `T`".
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_inherited_set_enforced_8649,
           override_omitting_where_resolves_an_operator_through_the_inherited_bound) {
    JitResult r = runAot(R"(
module M;
class B
  public virtual method Z<T>(a: T): T where T: INumber<T> => a + a;
end B;
class D: B
  public override method Z<T>(a: T): T => a + a;
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z(7));
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "is not available on type parameter"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "14\n");
}

// ---------------------------------------------------------------------------
// R11 — R10's operator body with a violating argument: the operator resolves
// and the CALL is rejected, exactly as for the written clause. Before the fix:
// rejected for the WRONG reason, in the body ("operator `+` is not available
// on type parameter `T`").
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_inherited_set_enforced_8649,
           override_operator_body_rejects_a_violating_argument_at_the_call) {
    JitResult r = runAot(R"(
module M;
class B
  public virtual method Z<T>(a: T): T where T: INumber<T> => a + a;
end B;
class D: B
  public override method Z<T>(a: T): T => a + a;
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z("x"));
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "is not available on type parameter"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `string` does not satisfy the constraint `where T: "
           "INumber`"));
}

// ---------------------------------------------------------------------------
// R12 — a method implementing TWO interfaces' members whose sets differ
// (`I1.Z where T: IA`, `I2.Z where T: IB`), called directly: every inherited
// set must hold, since a caller through either interface relies on its own.
// `OnlyA` satisfies `IA` but not `IB`. Before the fix: accepted, prints `7`.
// INTERPRETATION RECORDED in the commit body (§14.2 does not say what two
// differing inherited sets mean; the union is the direction that rejects).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_inherited_set_enforced_8649,
           two_implemented_interfaces_direct_call_enforces_every_inherited_set) {
    JitResult r = runAot(R"(
module M;
interface IA
  static method MakeA(): int;
end IA;
interface IB
  static method MakeB(): int;
end IB;
interface I1
  method Z<T>(a: T): int where T: IA;
end I1;
interface I2
  method Z<T>(a: T): int where T: IB;
end I2;
class OnlyA: IA
  public constructor()
  end constructor;
  public static method MakeA(): int => 1;
end OnlyA;
class D: I1, I2
  public constructor()
  end constructor;
  public method Z<T>(a: T): int => 7;
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z(new OnlyA()));
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `OnlyA` does not satisfy the constraint `where T: "
           "IB`"));
}

// ---------------------------------------------------------------------------
// R13 — the same two-interface method's BODY may rely only on a bound EVERY
// inherited set names: a call through an `I2` reference is checked against
// `I2`'s set alone, so `T.MakeA()` (from `I1`'s set only) would dispatch on a
// type argument that implements `IB` only. Before the fix (#8618 read the
// first interface's set alone): compiles, prints `0` (garbage).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_inherited_set_enforced_8649,
           two_implemented_interfaces_body_sees_only_the_common_bound) {
    JitResult r = runAot(R"(
module M;
interface IA
  static method MakeA(): int;
end IA;
interface IB
  static method MakeB(): int;
end IB;
interface I1
  method Z<T>(a: T): int where T: IA;
end I1;
interface I2
  method Z<T>(a: T): int where T: IB;
end I2;
class OnlyB: IB
  public constructor()
  end constructor;
  public static method MakeB(): int => 2;
end OnlyB;
class D: I1, I2
  public constructor()
  end constructor;
  public method Z<T>(a: T): int => T.MakeA();
end D;
class P
  public static method Main(): int
    var d := new D();
    var i: I2 := d;
    Console.WriteLine(i.Z(new OnlyB()));
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "has no static member `MakeA`"));
}

// ---------------------------------------------------------------------------
// R14 — the CROSS-MODULE imported base: a local `D` overrides an IMPORTED
// `B.Z<T> where T: IMaker`, omitting the clause; the inherited set comes from
// the module side table. `Q` does not implement `IMaker`. Before the fix:
// accepted, prints `5`.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_inherited_set_enforced_8649,
           imported_base_override_rejects_a_violating_argument) {
    std::vector<ModuleSource> mods;
    mods.push_back({"Dep", R"BZ(
module Dep;
public interface IMaker
  static method Make(): int;
end IMaker;
public class K: IMaker
  public constructor()
  end constructor;
  public static method Make(): int => 42;
end K;
public class B
  public constructor()
  end constructor;
  public virtual method Z<T>(a: T): int where T: IMaker => T.Make();
end B;
)BZ"});
    mods.push_back({"App", R"BZ(
module App;
import Dep;
class Q
  public constructor()
  end constructor;
end Q;
class D: B
  public constructor()
  end constructor;
  public override method Z<T>(a: T): int => 5;
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z(new Q()));
    return 0;
  end Main;
end P;
)BZ"});
    auto r = runAotModules(mods, "App");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `Q` does not satisfy the constraint `where T: "
           "IMaker`"));
}

// ---------------------------------------------------------------------------
// Boundary control (green on #8618's tip; it goes RED under the wrong fix
// named in its comment and in the commit body). The #8618 suite's admitted
// rows (renamed parameter, `D: B<Num>` substitution, interface implementation,
// chained omission) are the other must-still-admit boundary: each now also
// passes through this instantiation-site check.
// ---------------------------------------------------------------------------

// C1 — the generic class face with a CONFORMING argument stays admitted and
// runs. Wrong fix it catches: enforcing the inherited bound as written on the
// BASE (`IBox<X>`, the base's own parameter) instead of under the overridden
// base's substitution, which no argument satisfies.
BEZEL_TEST(sema_constraint_inherited_set_enforced_8649,
           control_generic_class_face_conforming_argument_still_runs) {
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
class B<X>
  public virtual method Z<S>(a: S): owned X where S: IBox<X> => S.Make();
end B;
class D<Y>: B<Y>
  public constructor()
  end constructor;
  public override method Z<S>(a: S): owned Y => S.Make();
end D;
class P
  public static method Main(): int
    var d := new D<Num>();
    using var n := d.Z<Num>(new Num(5));
    Console.WriteLine(n.V);
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}
