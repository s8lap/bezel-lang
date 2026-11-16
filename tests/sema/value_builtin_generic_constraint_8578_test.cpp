// Regression tests for issue #8578 — a generic constraint `where T:
// IComparable<T>` (or `where T: IEquatable<T>`) REFUSED the stdlib value
// builtins the STANDARD LIBRARY declares `: IComparable<Self>,
// IEquatable<Self>`.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * stdlib §2.5 (v0.357) declares the temporal value records
//       public record DateTime: IComparable<DateTime>, IEquatable<DateTime>
//       public record TimeSpan: IComparable<TimeSpan>, IEquatable<TimeSpan>
//       public record DateOnly: IComparable<DateOnly>, IEquatable<DateOnly>
//       public record TimeOnly: IComparable<TimeOnly>, IEquatable<TimeOnly>
//     and stdlib §2.6 (v0.357) declares
//       public record Guid: IComparable<Guid>, IEquatable<Guid>
//     ⇒ each of the five CONFORMS to `IComparable<Self>` / `IEquatable<Self>`
//     BY DECLARATION.
//
//   * spec §14.2 (v0.357) *Interface conformance* — "For an interface I and a
//     candidate type T, T conforms to I exactly when one of the following
//     holds": rule 2 admits "T is a record that lists I (or any interface
//     extending I) in its inheritance clause and declares matching members",
//     and rule 5 reads the SAME standard-library declaration for a primitive
//     ("stdlib §9 declares that type in record syntax, so `int` conforms to
//     `IComparable<int>` … exactly as clause 2 reads for records"). The stdlib
//     value builtins are records in that same sense, so rule 2 is their
//     conformance source.
//
//   * spec §14.2 (v0.357) *Type-parameter bounds*: "`where T: IFoo` admits
//     exactly the substitutions for which T conforms to `IFoo` per the rule
//     above." A `TimeSpan` substitution conforms ⇒ it is admitted.
//
// THE DEFECT (issue #8578, measured on `staging/r30-c1` = `0cb32bb14`).
//
// The compiler-intrinsic symbol a stdlib value builtin carries
// (`SemaContext::builtinClassSym`) is not a reification of the stdlib
// declaration: it has no `baseTypes`, so the §14.2 walk over the candidate's
// DECLARED bases reaches an empty closure and never finds `IComparable`. The
// call site then reports
//
//     type argument `TimeSpan` does not satisfy the constraint
//     `where T: IComparable` (§14.2)
//
// This is the §14.2 CONSTRAINT twin of the §5.1 CONVERSION path's #8288 arm
// (the conversion `TimeSpan` → `IComparable<TimeSpan>`), which #8288 already
// fixed. The two paths disagreed: the same `TimeSpan` converted to
// `IComparable<TimeSpan>` but was refused by `where T: IComparable<T>`.
//
// THE FIX.
//
// `argSatisfiesTypeBound` (src/sema/TypeResolution_Checks.cpp) now answers
// §14.2 rule 2 for the five stdlib value builtins: the intrinsic symbol is
// recognized by its own shape (`isValueBuiltin`, no `decl`, no `moduleName` —
// a USER type of the same name has a real `decl` or a populated
// `moduleName`), and the bound's type argument must be the candidate itself
// (the constraint's own type parameter, or the candidate spelled out). Any
// OTHER instantiation stays refused, because the stdlib declaration lists
// exactly `IComparable<Self>` and `IEquatable<Self>` and nothing else.
//
// ORACLE. `runAot` — the AOT path, the strict oracle. Every ACCEPTING row
// asserts the EXACT STDOUT (a VALUE oracle, never "no diagnostic was
// produced"); every REFUSING row asserts the SPECIFIC diagnostic text and
// that the refusal is a COMPILE-TIME refusal at a REAL location.
//
// THE CONTROLS are the positive twins of the fix's shape: a user record that
// declares `IComparable<Rec>` is still admitted, a user record that declares
// NO `IComparable` is still refused, a USER type named `TimeSpan` (which
// shadows the builtin) is still refused, a bound naming a DIFFERENT
// instantiation (`IComparable<DateTime>`) is still refused, and the
// `IEqualityComparer<T>` bound — which the stdlib declaration does NOT list
// for these types — is still refused. Each of those is green BEFORE the fix
// too: they are the rows that show the fix is narrow.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The program's stdout, verbatim. "<did-not-run>" when the program was
// rejected, crashed or timed out.
std::string out(const std::string& program) {
    JitResult r = runAot(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}

// The issue's own repro (#8578), verbatim in shape: a generic method
// constrained `where T: IComparable<T>`, called with the stdlib value builtin
// `TimeSpan`. The body prints "accepted" so the row has a VALUE oracle —
// "it compiled" alone would not see a fix that misreads its arguments.
const char* kIssueRepro =
    "module Conf;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    return F(TimeSpan.FromTicks(5L));\n"
    "  end Main;\n"
    "  public static method F<T>(x: T): int where T: IComparable<T>\n"
    "    Console.WriteLine(\"accepted\");\n"
    "    return 0;\n"
    "  end F;\n"
    "end Program;\n";

// The same shape with the OTHER of the two declared conformances.
const char* kIssueReproEquatable =
    "module Conf;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    return F(TimeSpan.FromTicks(5L));\n"
    "  end Main;\n"
    "  public static method F<T>(x: T): int where T: IEquatable<T>\n"
    "    Console.WriteLine(\"accepted\");\n"
    "    return 0;\n"
    "  end F;\n"
    "end Program;\n";

// The five stdlib value builtins that stdlib §2.5/§2.6 declare
// `: IComparable<Self>, IEquatable<Self>`, each in its own program. The
// candidate is the ROW'S DATA — the row reads the thing the fix handles, so
// a fix that admitted only `TimeSpan` would fail here.
std::string valueBuiltinProgram(const std::string& argument) {
    return "module Conf;\n"
           "public class Program\n"
           "  public static method Main(): int\n"
           "    return F(" + argument + ");\n"
           "  end Main;\n"
           "  public static method F<T>(x: T): int where T: IComparable<T>\n"
           "    Console.WriteLine(\"accepted\");\n"
           "    return 0;\n"
           "  end F;\n"
           "end Program;\n";
}

// The CONTROL: a user record that declares `IComparable<Rec>` is admitted —
// before the fix and after it.
const char* kUserRecordComparable =
    "module Conf;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    return F(new Rec());\n"
    "  end Main;\n"
    "  public static method F<T>(x: T): int where T: IComparable<T>\n"
    "    Console.WriteLine(\"accepted\");\n"
    "    return 0;\n"
    "  end F;\n"
    "end Program;\n"
    "public record Rec: IComparable<Rec>\n"
    "  public method CompareTo(other: Rec): int\n"
    "    return 0;\n"
    "  end CompareTo;\n"
    "end Rec;\n";

// The CONTROL: a user record that declares NO `IComparable` is refused.
const char* kUserRecordNotComparable =
    "module Conf;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    return F(new Rec());\n"
    "  end Main;\n"
    "  public static method F<T>(x: T): int where T: IComparable<T>\n"
    "    Console.WriteLine(\"accepted\");\n"
    "    return 0;\n"
    "  end F;\n"
    "end Program;\n"
    "public record Rec\n"
    "  public method N(): int\n"
    "    return 1;\n"
    "  end N;\n"
    "end Rec;\n";

// The CONTROL: a USER type named `TimeSpan` — the same simple name as the
// stdlib value builtin — declares NO `IComparable` and is still refused. The
// fix recognizes the BUILTIN by the intrinsic symbol's shape, so a
// same-named user type must not slip in.
const char* kUserTypeNamedTimeSpan =
    "module Conf;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    return F(new TimeSpan());\n"
    "  end Main;\n"
    "  public static method F<T>(x: T): int where T: IComparable<T>\n"
    "    Console.WriteLine(\"accepted\");\n"
    "    return 0;\n"
    "  end F;\n"
    "end Program;\n"
    "public record TimeSpan\n"
    "  public method N(): int\n"
    "    return 1;\n"
    "  end N;\n"
    "end TimeSpan;\n";

// The bound spells the CANDIDATE's own type out — the same instantiation the
// stdlib declaration names (`IComparable<TimeSpan>` for a `TimeSpan`).
const char* kSpelledSelfInstantiation =
    "module Conf;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    return F(TimeSpan.FromTicks(5L));\n"
    "  end Main;\n"
    "  public static method F<T>(x: T): int where T: IComparable<TimeSpan>\n"
    "    Console.WriteLine(\"accepted\");\n"
    "    return 0;\n"
    "  end F;\n"
    "end Program;\n";

// The CONTROL: the bound names a DIFFERENT instantiation. `TimeSpan`
// conforms to `IComparable<TimeSpan>` (stdlib §2.5), NOT to
// `IComparable<DateTime>` — so this stays refused.
const char* kWrongInstantiation =
    "module Conf;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    return F(TimeSpan.FromTicks(5L));\n"
    "  end Main;\n"
    "  public static method F<T>(x: T): int where T: IComparable<DateTime>\n"
    "    Console.WriteLine(\"accepted\");\n"
    "    return 0;\n"
    "  end F;\n"
    "end Program;\n";

// The CONTROL: `IEqualityComparer<T>` is NOT one of the two conformances the
// stdlib declaration lists for these types (stdlib §2.8/§4.5 declare it as an
// equality-COMPARER agent), so the bound stays refused.
const char* kOtherStdlibInterface =
    "module Conf;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    return F(TimeSpan.FromTicks(5L));\n"
    "  end Main;\n"
    "  public static method F<T>(x: T): int where T: IEqualityComparer<T>\n"
    "    Console.WriteLine(\"accepted\");\n"
    "    return 0;\n"
    "  end F;\n"
    "end Program;\n";

// The §14.2 refusal text every refusing row must carry.
const char* kConstraintRefusal =
    "does not satisfy the constraint `where T: IComparable` (§14.2)";

}  // namespace

// ---------------------------------------------------------------------------
// ROW 1 — the issue's own repro. `where T: IComparable<T>` with a `TimeSpan`
// argument must be ADMITTED and the program must print "accepted".
//
// Pre-fix: "type argument `TimeSpan` does not satisfy the constraint
// `where T: IComparable` (§14.2)" — the program is refused and never runs.
// ---------------------------------------------------------------------------
BEZEL_TEST(value_builtin_generic_constraint_8578,
           issue_repro_admits_timespan_against_icomparable) {
    BEZEL_EXPECT_EQ(out(kIssueRepro), "accepted\n");
}

// ---------------------------------------------------------------------------
// ROW 2 — the same shape against the OTHER declared conformance,
// `where T: IEquatable<T>`.
// ---------------------------------------------------------------------------
BEZEL_TEST(value_builtin_generic_constraint_8578,
           issue_repro_admits_timespan_against_iequatable) {
    BEZEL_EXPECT_EQ(out(kIssueReproEquatable), "accepted\n");
}

// ---------------------------------------------------------------------------
// ROW 3 — EVERY stdlib value builtin that stdlib §2.5/§2.6 declares
// `: IComparable<Self>`: DateTime, TimeSpan, DateOnly, TimeOnly and Guid.
// The candidate is the ROW'S DATA, so a fix that admitted only one of them
// fails here.
// ---------------------------------------------------------------------------
BEZEL_TEST(value_builtin_generic_constraint_8578,
           admits_every_stdlib_comparable_value_builtin) {
    BEZEL_EXPECT_EQ(
        out(valueBuiltinProgram("DateTime.FromTicks(0L)")), "accepted\n");
    BEZEL_EXPECT_EQ(
        out(valueBuiltinProgram("TimeSpan.FromTicks(5L)")), "accepted\n");
    BEZEL_EXPECT_EQ(
        out(valueBuiltinProgram("DateOnly.Parse(\"2020-01-02\")")),
        "accepted\n");
    BEZEL_EXPECT_EQ(
        out(valueBuiltinProgram("TimeOnly.Parse(\"01:02:03\")")), "accepted\n");
    BEZEL_EXPECT_EQ(out(valueBuiltinProgram("Guid.Empty")), "accepted\n");
}

// ---------------------------------------------------------------------------
// ROW 4 — the bound SPELLS the candidate's own type out
// (`where T: IComparable<TimeSpan>` with a `TimeSpan` argument). The stdlib
// declaration names exactly that instantiation, so it is admitted.
//
// Pre-fix: refused with the §14.2 diagnostic (the walk over the intrinsic
// symbol's declared bases reaches an empty closure).
// ---------------------------------------------------------------------------
BEZEL_TEST(value_builtin_generic_constraint_8578,
           the_spelled_self_instantiation_is_admitted) {
    BEZEL_EXPECT_EQ(out(kSpelledSelfInstantiation), "accepted\n");
}

// ---------------------------------------------------------------------------
// ROW 5 — the CONTROL: a user record that declares `IComparable<Rec>` is
// still admitted (this row is green before the fix too — it is the positive
// twin of the fix's shape).
// ---------------------------------------------------------------------------
BEZEL_TEST(value_builtin_generic_constraint_8578,
           user_record_declaring_icomparable_is_still_admitted) {
    BEZEL_EXPECT_EQ(out(kUserRecordComparable), "accepted\n");
}

// ---------------------------------------------------------------------------
// ROW 6 — the CONTROL: a user record that declares NO `IComparable` is still
// REFUSED, with the §14.2 diagnostic (green before the fix too).
// ---------------------------------------------------------------------------
BEZEL_TEST(value_builtin_generic_constraint_8578,
           user_record_without_icomparable_is_still_refused) {
    JitResult r = runAot(kUserRecordNotComparable);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kConstraintRefusal));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ---------------------------------------------------------------------------
// ROW 7 — the CONTROL: a USER type named `TimeSpan` (shadowing the builtin)
// declares NO `IComparable` and is still REFUSED. The fix recognizes the
// builtin by the intrinsic symbol's shape, so a same-named user type must not
// slip in.
// ---------------------------------------------------------------------------
BEZEL_TEST(value_builtin_generic_constraint_8578,
           user_type_named_timespan_is_still_refused) {
    JitResult r = runAot(kUserTypeNamedTimeSpan);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kConstraintRefusal));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ---------------------------------------------------------------------------
// ROW 8 — the CONTROL: a bound naming a DIFFERENT instantiation
// (`IComparable<DateTime>`) is still REFUSED. `TimeSpan` conforms to
// `IComparable<TimeSpan>`, not to `IComparable<DateTime>`.
// ---------------------------------------------------------------------------
BEZEL_TEST(value_builtin_generic_constraint_8578,
           a_different_icomparable_instantiation_is_still_refused) {
    JitResult r = runAot(kWrongInstantiation);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kConstraintRefusal));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ---------------------------------------------------------------------------
// ROW 9 — the CONTROL: the `IEqualityComparer<T>` bound, which the stdlib
// declaration does NOT list for these types, is still REFUSED.
// ---------------------------------------------------------------------------
BEZEL_TEST(value_builtin_generic_constraint_8578,
           an_interface_the_stdlib_does_not_declare_is_still_refused) {
    JitResult r = runAot(kOtherStdlibInterface);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "does not satisfy the constraint `where T: IEqualityComparer` "
           "(§14.2)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}
