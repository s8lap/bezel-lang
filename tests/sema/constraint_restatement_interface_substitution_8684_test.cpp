// fix #8684 (spec §14.2 *Constraints on overrides and interface
// implementations*, v0.357): the INHERITED constraint set of an interface
// implementation is read UNDER THE IMPLEMENTED INTERFACE'S OWN
// TYPE-ARGUMENT SUBSTITUTION, so a `where` clause that RESTATES it in the
// substituted form restates the SAME set and is legal.
//
// SPEC BASIS (QUOTED, v0.357 — spec §14.2, "Constraints on overrides and
// interface implementations").
//   "Omitting the `where` clauses is the recommended spelling and means
//    exactly the inherited set; the clauses may instead be restated in an
//    equivalent form — the same constraint set per type parameter, compared
//    order-insensitively and after type-alias resolution (§27.8): `where T:
//    IFoo` and `where T: AliasOfIFoo` restate the same set — for
//    readability."
//   "The two declarations' method-level type parameters, equal in number,
//    correspond by position (§11.2's *Generic members*), and the inherited
//    set is read under the implemented interface's or overridden base's
//    substitution, its method type parameters renamed positionally: `method
//    M<S>(x: S) where S: U;` on `interface I<U>` is inherited as `where T:
//    IFoo` by `method M<T>(x: T)` in a class conforming to `I<IFoo>`."
//
// THE DEFECT. The interface-implementation restatement check compared the RAW
// interface clause with the implementation's, so `where S: IBox<Num>` on
// `class D: IBox<Num>` was compared against the interface's `where S:
// IBox<T>` — a DIFFERENT set, because T was never substituted by Num — and
// the DECLARATION was rejected:
//
//   "method `Z` implements interface member `Z` and must restate an
//    equivalent `where` clause — omit the clause to inherit it exactly, or
//    restate the identical set (§14.2)"
//
// while the very same program with the clause OMITTED (the recommended
// spelling) compiles and runs. §14.2's own example is the shape that was
// over-rejected: `method M<S>(x: S) where S: U;` on `interface I<U>` is
// inherited as `where T: IFoo` by `method M<T>(x: T)` in a class conforming
// to `I<IFoo>`.
//
// Both rows below FAIL without the fix (the base rejects the declaration) and
// PASS with it.

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"        // runAot
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

// ---------------------------------------------------------------------------
// R1 — THE ISSUE'S REPRO (verbatim): `class D: IBox<Num>` RESTATES the
// interface method's clause in the substituted form `where S: IBox<Num>`,
// which IS `IBox<T>`'s `where S: IBox<T>` under T := Num — the same set.
//
// Before the fix: the declaration is rejected ("method `Z` implements
// interface member `Z` and must restate an equivalent `where` clause — omit
// the clause to inherit it exactly, or restate the identical set (§14.2)") and
// the program never runs.
// With the fix: the program compiles, runs, and prints `1`.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_restatement_under_interface_substitution_8684,
           issue_repro_restated_clause_under_the_interface_substitution_is_admitted) {
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
  public method Z<S>(a: S): owned Num where S: IBox<Num> => S.Make();
  public static method Make(): owned Num => new Num(3);
end D;
class P
  public static method Main(): int
    var d := new D();
    using var n := new Num(1);
    Console.WriteLine(n.V);
    return 0;
  end Main;
end P;
)");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}

// ---------------------------------------------------------------------------
// R2 — the restated clause IS the interface's clause, not merely tolerated:
// it is ENFORCED at the call, exactly as the omitted clause's inherited set
// is. `Num` does not implement `IBox<Num>`, so `d.Z<Num>(...)` is rejected at
// the call site with the constraint's own diagnostic.
//
// This is #8649's R4 program with the clause RESTATED instead of omitted.
// Before the fix: the program is rejected at the DECLARATION (the
// restatement diagnostic), so the call-site diagnostic is never reached and
// this row fails.
// With the fix: the declaration is admitted and the CALL is rejected with the
// constraint diagnostic.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_constraint_restatement_under_interface_substitution_8684,
           restated_clause_is_the_interface_clause_and_is_enforced_at_the_call) {
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
  public method Z<S>(a: S): owned Num where S: IBox<Num> => S.Make();
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
    // The DECLARATION is admitted — the restatement is NOT rejected any more.
    // (Without the fix the base rejects the declaration with exactly this
    // diagnostic, and the row fails here.)
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "must restate an equivalent"));
    // And the restated clause IS the interface's clause: it is enforced at
    // the call.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `Num` does not satisfy the constraint `where S: "
           "IBox`"));
}
