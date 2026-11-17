// fix #8590 / spec §14.2 *Interface conformance* rule 2 and *Worked example:
// `where T: IComparable<int>`* (v0.357): a type-bound constraint names an
// interface INSTANTIATION, and a BUILT-IN type name spelled in the bound is
// that instantiation — NOT the constraint's own type parameter.
//
// WHAT THE SPEC REQUIRES.
//
//   * §14.2 *Interface conformance* (v0.357): "For an interface I and a
//     candidate type T, T conforms to I exactly when one of the following
//     holds:" — rule 2 admits "T is a record that lists I (or any interface
//     extending I) in its inheritance clause and declares matching members".
//     The `I` of rule 2 is the bound's INSTANTIATION: a record conforming to
//     `IComparable<Rec>` does NOT conform to `IComparable<DateTime>`.
//   * §14.2 *Worked example: `where T: IComparable<int>`* (v0.357): "`IComparable<int>`
//     is implemented by `int` itself ...; by a user record listing
//     `IComparable<int>` in its inheritance clause **and declaring the matching
//     members** (rule 2 ...); by a user class implementing it (rule 1); and by
//     an interface `IIntComparable: IComparable<int>` (rule 3)."
//     The same holds for every other instantiation: the bound names ONE
//     instantiation, and only a candidate listing THAT instantiation conforms.
//   * §14.2 (v0.357), the type-confusion rule fix #4863 closed (v0.307):
//     "where T: IEquatable<Other>` requires T to conform to THAT
//     instantiation, not merely to 'some instantiation of IEquatable'" — the
//     body's `a.Equals(o)` is then called with an `Other`-typed argument
//     against a `Self`-typed member.
//
// THE DEFECT (issue #8590; MEASURED on `staging/r30-c1` = `5e14cf2c6`).
//
// `argSatisfiesTypeBound` (src/sema/TypeResolution_Checks.cpp) asked "is the
// bound's type argument a bare-parameter marker?" with
//
//     if (bArg && bArg->kind == TypeRef::Kind::Named && !bArg->declaration)
//
// `!bArg->declaration` is true for TWO shapes: a bare-parameter marker
// (`where T: IComparable<T>` — the constraint's own parameter) and a
// DECLARATION-LESS BUILT-IN name spelled in the bound (`where T:
// IComparable<DateTime>`). A built-in name keeps `declaration == nullptr`
// (the resolver admits the name, `Resolver::builtinTypeArity`), so the bound's
// argument was read as the constraint's own parameter and REPLACED by the
// candidate — the instantiation check then compared the candidate with ITSELF
// and the WRONG instantiation was admitted.
//
// THE FIX (src/sema/TypeResolution_Checks.cpp, `argSatisfiesTypeBound` only).
//
//   * `isBoundParamMarker` — a declaration-less Named ref in a bound's
//     instantiation position is the constraint's own parameter only when the
//     RESOLVER says so (its own `TypeRef::typeParam` record, #8213: `Param` =
//     the spelling bound to a type parameter, `NotParam` = the spelling
//     resolved to a concrete type), and otherwise only when the name is not a
//     built-in type name (`isOpenParamLeaf`). A built-in name in the bound is
//     therefore the INSTANTIATION it spells.
//   * `sameBoundInstantiationArg` — that instantiation is compared with the
//     CANDIDATE's own instantiation of the same interface, by NAME where both
//     are declaration-less (`Resolver::typeRefEqual` equates any two
//     declaration-less refs, the blind spot #6315 documents).
//
// ORACLE. `runAot` — the AOT path, the strict oracle. Every REFUSING row
// asserts the SPECIFIC §14.2 diagnostic AND that the refusal is a compile-time
// refusal at a REAL location; the ADMITTING row asserts the program's exact
// stdout, so "it compiled" is never the oracle.
//
// EVERY ROW BELOW FAILS WITHOUT THE FIX (each is RED on the base compiler) and
// passes with it. The base-green shapes — a bound naming a USER type (already
// refused) and a built-in bound that IS satisfied (`where T:
// IComparable<DateTime>` with `T := DateTime`) — are conformance-corpus
// candidates, not gtest rows (the owner's cap: a gtest row must fail without
// the fix); they live in the lane's probe directory with the deciding spec
// sentence.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The issue's own repro, verbatim: a generic method constrained
// `where T: IComparable<DateTime>`, called with a USER record that declares
// only `IComparable<Rec>`. The body prints "accepted" so the pre-fix
// behaviour has a VALUE oracle ("it compiled" alone would not see a fix that
// misreads the bound's argument).
const char* kIssueRepro =
    "module Conf;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    return F(new Rec());\n"
    "  end Main;\n"
    "  public static method F<T>(x: T): int where T: IComparable<DateTime>\n"
    "    Console.WriteLine(\"accepted\");\n"
    "    return 0;\n"
    "  end F;\n"
    "end Program;\n"
    "public record Rec: IComparable<Rec>\n"
    "  public method CompareTo(other: Rec): int\n"
    "    return 0;\n"
    "  end CompareTo;\n"
    "end Rec;\n";

// The SAME shape with a CLASS candidate — §14.2 rule 1 (a class that lists I
// in its inheritance clause) is the same instantiation rule, so the class path
// must refuse the same way the record path does.
const char* kClassCandidate =
    "module Conf;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    return F(new RecC());\n"
    "  end Main;\n"
    "  public static method F<T>(x: T): int where T: IComparable<DateTime>\n"
    "    Console.WriteLine(\"accepted\");\n"
    "    return 0;\n"
    "  end F;\n"
    "end Program;\n"
    "public class RecC: IComparable<RecC>\n"
    "  public constructor() end constructor;\n"
    "  public method CompareTo(other: RecC): int\n"
    "    return 0;\n"
    "  end CompareTo;\n"
    "end RecC;\n";

// The bound names a DIFFERENT built-in instantiation (`IComparable<Guid>`).
// `Rec` conforms to `IComparable<Rec>` — §14.2 rule 2 — and to nothing else,
// so this stays refused.
const char* kOtherBuiltinInstantiation =
    "module Conf;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    return F(new Rec());\n"
    "  end Main;\n"
    "  public static method F<T>(x: T): int where T: IComparable<Guid>\n"
    "    Console.WriteLine(\"accepted\");\n"
    "    return 0;\n"
    "  end F;\n"
    "end Program;\n"
    "public record Rec: IComparable<Rec>\n"
    "  public method CompareTo(other: Rec): int\n"
    "    return 0;\n"
    "  end CompareTo;\n"
    "end Rec;\n";

// THE ADMITTING TWIN: the same shape, with the candidate conforming to the
// instantiation the bound NAMES — the record lists `IComparable<DateTime>` in
// its inheritance clause and declares the matching `CompareTo` member, which
// is §14.2 rule 2 exactly. This row is the fix's positive direction: it must
// be ADMITTED (and it is REFUSED on the base, which compared the candidate
// with itself and so compared the candidate `Rec2` with the bound's
// `DateTime`).
const char* kConformingToTheNamedInstantiation =
    "module Conf;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    return F(new Rec2());\n"
    "  end Main;\n"
    "  public static method F<T>(x: T): int where T: IComparable<DateTime>\n"
    "    Console.WriteLine(\"accepted\");\n"
    "    return 0;\n"
    "  end F;\n"
    "end Program;\n"
    "public record Rec2: IComparable<DateTime>\n"
    "  public method CompareTo(other: DateTime): int\n"
    "    return 0;\n"
    "  end CompareTo;\n"
    "end Rec2;\n";

// The program's stdout, verbatim. "<did-not-run>" when the program was
// rejected, crashed or timed out.
std::string out(const std::string& program) {
    JitResult r = runAot(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}

// The §14.2 refusal text every refusing row must carry. The instantiation-site
// diagnostic names the bound's DECLARATION (its generic arguments are
// deliberately dropped — the §14.2 diagnostic is written for the common case).
const char* kConstraintRefusal =
    "does not satisfy the constraint `where T: IComparable` (§14.2)";

}  // namespace

// ---------------------------------------------------------------------------
// ROW 1 — the issue's own repro. `Rec` conforms to `IComparable<Rec>`
// (§14.2 rule 2), not to `IComparable<DateTime>`, so the call is a compile
// error.
//
// Pre-fix: the program COMPILES, prints "accepted" and exits 0.
// ---------------------------------------------------------------------------
BEZEL_TEST(builtin_bound_constraint_8590,
           record_conforming_to_icomparable_rec_is_refused_against_icomparable_datetime) {
    JitResult r = runAot(kIssueRepro);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kConstraintRefusal));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ---------------------------------------------------------------------------
// ROW 2 — the CLASS path. §14.2 rule 1 admits a class that lists the
// interface in its inheritance clause; the instantiation is the bound's, so a
// class conforming to `IComparable<RecC>` does not satisfy
// `where T: IComparable<DateTime>` either.
//
// Pre-fix: the program COMPILES, prints "accepted" and exits 0.
// ---------------------------------------------------------------------------
BEZEL_TEST(builtin_bound_constraint_8590,
           class_conforming_to_icomparable_recc_is_refused_against_icomparable_datetime) {
    JitResult r = runAot(kClassCandidate);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kConstraintRefusal));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ---------------------------------------------------------------------------
// ROW 3 — the bound names a DIFFERENT built-in instantiation. `Guid` is a
// stdlib value builtin (stdlib §2.6), so the bound names
// `IComparable<Guid>` — an instantiation `Rec` does not conform to.
//
// Pre-fix: the program COMPILES, prints "accepted" and exits 0.
// ---------------------------------------------------------------------------
BEZEL_TEST(builtin_bound_constraint_8590,
           a_different_builtin_instantiation_in_the_bound_is_refused) {
    JitResult r = runAot(kOtherBuiltinInstantiation);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kConstraintRefusal));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ---------------------------------------------------------------------------
// ROW 4 — THE ADMITTING TWIN: the candidate lists the instantiation the bound
// NAMES (`IComparable<DateTime>`) and declares the matching member, which is
// §14.2 rule 2 exactly. The bound is satisfied, so the call is ADMITTED and
// the program prints "accepted".
//
// Pre-fix: the program is REFUSED — the base replaced the bound's argument
// with the candidate and then compared the candidate `Rec2` with the bound's
// `DateTime`.
// ---------------------------------------------------------------------------
BEZEL_TEST(builtin_bound_constraint_8590,
           a_candidate_conforming_to_the_named_builtin_instantiation_is_admitted) {
    BEZEL_EXPECT_EQ(out(kConformingToTheNamedInstantiation), "accepted\n");
}
