// fix #8618 (spec §14.2 *Constraints on overrides and interface
// implementations*, v0.357): an `override` or an interface implementation that
// OMITS its `where` clause INHERITS the overridden/implemented declaration's
// constraint set, so a static-interface-member call through the inherited
// bound (`T.Zero()`) is admitted.
//
// SPEC BASIS (QUOTED, v0.357 — spec §14.2, the subsection
// "Constraints on overrides and interface implementations").
//   "An `override` of a generic method, and a method implementing a generic
//   interface method (an interface member declaring type parameters of its own,
//   whose `where` clauses sit where a class method's do: Appendix A.4
//   `IfaceMethod`, `IfaceStaticMethod`), inherits the overridden or implemented
//   declaration's constraints. Omitting the `where` clauses is the recommended
//   spelling and means exactly the inherited set; the clauses may instead be
//   restated in an equivalent form — the same constraint set per type
//   parameter, compared order-insensitively and after type-alias resolution
//   (§27.8)".
//   "The two declarations' method-level type parameters, equal in number,
//   correspond by position (§11.2's *Generic members*), and the inherited set
//   is read under the implemented interface's or overridden base's
//   substitution, its method type parameters renamed positionally".
//
// AND §11.8 (the lookup this fix reaches): "`T.Member(...)` dispatch requires
// a generic type **parameter** constrained by the interface" — the call is
// admitted when the parameter's constraint set names an interface that declares
// the static member (stdlib §2.19's `INumber<T>` declares
// `static method Zero(): owned T;`).
//
// BEFORE THE FIX: "type parameter `T` has no static member `Zero` — `T.Member
// (...)` requires a `where T: <interface>` constraint whose interface declares
// that static member (§11.8)" — the §11.8 lookup read only the AST-WRITTEN
// `where` clauses, so the recommended spelling (the clause omitted) saw no
// bound at all.
//
// The rows below are the fix rows (each FAILS without the fix — the program is
// rejected, so `r.ran` is false) plus the two boundary controls (each is
// base-green and must STAY rejected; they go RED under the plausible wrong
// fixes named in the commit body).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

// The §11.8 needle the pre-fix compiler emitted for the rows below.
const char* kNoStaticMember = "has no static member";

}  // namespace

// ---------------------------------------------------------------------------
// R1 — THE ISSUE'S REPRO (verbatim, with a `Main` that prints so the row
// pins the RESULT and not only the acceptance).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_inherited_static_member_8618,
           override_omitting_where_sees_the_inherited_bound_for_a_static_call) {
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
    Console.WriteLine(d.Z(7));
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNoStaticMember));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "0\n");
}

// ---------------------------------------------------------------------------
// R2 — the RESULT TYPE through the inherited bound: `One()` is a distinct
// value, so a row that admitted the call but mistyped its result would print
// the wrong number.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_inherited_static_member_8618,
           inherited_bound_static_call_keeps_the_overrides_own_result_type) {
    JitResult r = runAot(R"(
module M;
class B
  public virtual method Z<T>(a: T): T where T: INumber<T> => T.One();
end B;
class D: B
  public override method Z<T>(a: T): T => T.One();
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z(7));
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNoStaticMember));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}

// ---------------------------------------------------------------------------
// R3 — the inherited set is read "with its method type parameters renamed
// positionally" (§14.2): the base names its parameter `S`, the override names
// its own `T`.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_inherited_static_member_8618,
           renamed_override_parameter_inherits_the_bound_by_position) {
    JitResult r = runAot(R"(
module M;
class B
  public virtual method Z<S>(a: S): S where S: INumber<S> => S.Zero();
end B;
class D: B
  public override method Z<T>(a: T): T => T.Zero();
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z(7));
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNoStaticMember));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "0\n");
}

// ---------------------------------------------------------------------------
// R4 — the interface-IMPLEMENTATION half of the §14.2 sentence: a method
// implementing a generic interface method inherits that method's constraint
// set when it omits its clause.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_inherited_static_member_8618,
           interface_implementation_omitting_where_sees_the_inherited_bound) {
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
    Console.WriteLine(d.Z(7));
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNoStaticMember));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "0\n");
}

// ---------------------------------------------------------------------------
// R5 — a CHAINED omission: the intermediate base class omits its clause too
// (legal at any level, §14.2), so the set is inherited two levels up.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_inherited_static_member_8618,
           chained_omission_inherits_the_bound_through_the_intermediate_base) {
    JitResult r = runAot(R"(
module M;
abstract class A
  public abstract method Z<T>(a: T): T where T: INumber<T>;
end A;
abstract class M2: A
  public override method Z<T>(a: T): T => T.Zero();
end M2;
class D: M2
  public constructor()
  end constructor;
  public override method Z<T>(a: T): T => T.Zero();
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z(7));
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNoStaticMember));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "0\n");
}

// ---------------------------------------------------------------------------
// R6 — the inherited set is read "under the implemented interface's or
// overridden base's substitution" (§14.2): the base's OWN class type parameter
// is substituted by the inheriting type's base-clause argument, so
// `class D: B<Num>` reads the base's `where S: IBox<X>` as `where S: IBox<Num>`.
// The row pins the ARGUMENT too: `IBox<Num>.Make()` returns `owned Num`, and
// the program prints the `V` of the `Num` the static member built.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_inherited_static_member_8618,
           inherited_bound_is_read_under_the_overridden_base_substitution) {
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
class D: B<Num>
  public constructor()
  end constructor;
  public override method Z<S>(a: S): owned Num => S.Make();
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
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNoStaticMember));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}

// ---------------------------------------------------------------------------
// C1 — CONTROL (base-green, must stay rejected): a method that overrides
// NOTHING and constrains nothing has no bound, so the §11.8 diagnostic still
// fires. This row goes RED under the plausible wrong fix "admit the static
// member whenever the receiver is a type parameter".
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_inherited_static_member_8618,
           an_unconstrained_parameter_still_has_no_static_member) {
    JitResult r = runAot(R"(
module M;
class C
  public method Z<T>(a: T): T => T.Zero();
end C;
class P
  public static method Main(): int
    var c := new C();
    Console.WriteLine(c.Z(7));
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNoStaticMember));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// C2 — CONTROL (base-green, must stay rejected): the base declares a
// SAME-NAMED SIBLING overload that carries the constraint, but the member this
// method overrides does NOT — a sibling is a distinct member (§12.2) and
// contributes nothing. This row goes RED under the plausible wrong fix
// "read the base closure's constraint set without the resolver's own
// override/implementation relation".
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_inherited_static_member_8618,
           a_same_named_sibling_overload_in_the_base_is_not_inherited) {
    JitResult r = runAot(R"(
module M;
class B
  public virtual method Z<T>(a: string): T where T: INumber<T> => T.Zero();
  public virtual method Z<T>(a: T): T => a;
end B;
class D: B
  public override method Z<T>(a: T): T => T.Zero();
end D;
class P
  public static method Main(): int
    var d := new D();
    Console.WriteLine(d.Z(7));
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kNoStaticMember));
    BEZEL_EXPECT_FALSE(r.ran);
}
