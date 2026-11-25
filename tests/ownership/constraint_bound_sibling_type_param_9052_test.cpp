// Regression test for issue #9052 — a GENERIC TYPE's `where` clause whose
// bound names ANOTHER type parameter of the same declaration
// (`class Repo<K, T> where T: IEntity<K>`) was REFUSED at every
// instantiation, because the bound was compared against the argument's
// implemented interfaces WITHOUT the instantiation's own arguments being
// substituted into it.
//
// THE ISSUE'S OWN REPRO (verbatim):
//
//   module P;
//   public interface IEntity<K>
//     readonly property Id: K;
//   end IEntity;
//   public class Repo<K, T> where T: IEntity<K>
//     public method IdOf(x: T): K => x.Id;
//   end Repo;
//   public class User(id: int): IEntity<int>
//     public readonly property Id: int := id;
//   end User;
//   public class Program
//     public static method Main()
//       using var r := new Repo<int, User>();
//       using var u := new User(4);
//       Console.WriteLine(r.IdOf(u));
//     end Main;
//   end Program;
//
// MEASURED on the base (`r30-c1` @ 41177f298):
//
//   probe.bzl:13:24: type argument `User` does not satisfy the constraint
//   `where T: IEntity` (§14.2)
//
// — the instantiation `Repo<int, User>` was refused although `User` lists
// `IEntity<int>` in its inheritance clause, and the diagnostic DROPPED the
// bound's type argument (`IEntity`, not `IEntity<int>`).
//
// THE ROOT CAUSE. `Resolver::checkOneConstraint`
// (src/sema/TypeResolution_Checks.cpp) resolves the bound and hands it to
// `argSatisfiesTypeBound` UNCHANGED. The bound is `IEntity<K>`, where `K` is
// the DECLARING generic's own type parameter — resolved as a bare-parameter
// marker (a Named TypeRef with no declaration). The conformance walk read
// such a marker as "the constraint's OWN parameter" (the `where T: IFoo<T>`
// shape, where the marker substitutes to the candidate) and compared it with
// the candidate's own instantiation `IEntity<int>`: `K` against `int` — never
// equal. The source documented the sibling shape as a gap.
//
// THE SPEC. §14.1 (v0.359) *Instantiation and its site*: "An **instantiation**
// of a generic declaration is the constructed type or constructed member
// obtained by substituting type arguments for its type parameters." §14.2
// (v0.359) *Constraints*: "where T: IFoo | Non-nullable types conforming to
// IFoo: classes implementing it, …" and *Interface conformance* rule 1: "**T
// is a class** that lists I (or any interface extending I) in its inheritance
// clause" — `I` being the bound AS INSTANTIATED. `Repo<int, User>` is the
// instantiation that substitutes `K := int`, so the bound it states is
// `IEntity<int>`, and `User` conforms to it by rule 1. The bound is read at
// the instantiation site under the full substitution; the diagnostic names
// that bound.
//
// ROWS.
//   Row 1 — the issue's own repro, verbatim: `Repo<int, User>` is ADMITTED and
//           the program prints `4`. RED on the base (the instantiation is
//           refused), GREEN with the fix.
//   Row 2 — the NEGATIVE: `Repo<string, User>` must still be REFUSED (the
//           `User` does not list `IEntity<string>`), and the diagnostic must
//           name the FULL SUBSTITUTED bound `IEntity<string>`. RED on the
//           base (the base prints `IEntity` — the bound's type argument
//           dropped), GREEN with the fix.
//
// per §14.1, §14.2, §3.6 rule 2 (issue #9052)

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The program ran to completion: it compiled, it exited normally, and it
// printed exactly the expected output.
void expectClean(const JitResult& r, int exitCode, const std::string& out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, exitCode);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// ── Row 1 (the issue's own repro / §14.1 / §14.2): the instantiation
// `Repo<int, User>` is ADMITTED and the program prints `4`.
//
// RED on the base — "type argument `User` does not satisfy the constraint
// `where T: IEntity` (§14.2)" — GREEN with the fix.
BEZEL_TEST(constraint_bound_sibling_type_param_9052,
           issue_repro_repo_int_user_is_admitted_and_prints_4) {
    const std::string src =
        "module P; "
        "public interface IEntity<K> "
        "  readonly property Id: K; "
        "end IEntity; "
        "public class Repo<K, T> where T: IEntity<K> "
        "  public method IdOf(x: T): K => x.Id; "
        "end Repo; "
        "public class User(id: int): IEntity<int> "
        "  public readonly property Id: int := id; "
        "end User; "
        "public class Program "
        "  public static method Main() "
        "    using var r := new Repo<int, User>(); "
        "    using var u := new User(4); "
        "    Console.WriteLine(r.IdOf(u)); "
        "  end Main; "
        "end Program;";
    expectClean(runAot(src), 0, "4\n");
}

// ── Row 2 (§14.2 *Interface conformance* rule 1): the NEGATIVE — the same
// declaration instantiated with a type argument the candidate does NOT
// conform to (`Repo<string, User>`: `User` lists `IEntity<int>`, not
// `IEntity<string>`) must still be REFUSED, and the diagnostic must name the
// bound AS IT STANDS AT THAT SITE — `IEntity<string>`, the instantiation's
// own argument substituted into the bound (§14.1), not the head name alone.
//
// RED on the base — the base refuses it too, but prints `where T: IEntity`
// (the bound's type argument DROPPED) — GREEN with the fix.
BEZEL_TEST(constraint_bound_sibling_type_param_9052,
           negative_repo_string_user_is_refused_naming_the_substituted_bound) {
    const std::string src =
        "module P; "
        "public interface IEntity<K> "
        "  readonly property Id: K; "
        "end IEntity; "
        "public class Repo<K, T> where T: IEntity<K> "
        "  public method IdOf(x: T): K => x.Id; "
        "end Repo; "
        "public class User(id: int): IEntity<int> "
        "  public readonly property Id: int := id; "
        "end User; "
        "public class Program "
        "  public static method Main() "
        "    using var r := new Repo<string, User>(); "
        "    using var u := new User(4); "
        "    Console.WriteLine(r.IdOf(u)); "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    // The refusal names the SUBSTITUTED bound: `K := string` at this site.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "does not satisfy the constraint `where T: IEntity<string>`"));
    // The bound's type argument is not dropped any more.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "IEntity<string>"));
}
