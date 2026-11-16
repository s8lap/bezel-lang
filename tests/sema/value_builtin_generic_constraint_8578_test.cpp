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
// THE LOWERING HALF (fix-forward after review r42-rev-m31-8578). §14.2 also
// says "Inside the generic body, T's surface includes IFoo's members", so an
// admitted substitution must answer the constraint's members with its own
// value's answer. The admission alone compiled `a.Equals(b)` through
// `T: IEquatable<T>` to a constant `False` for TimeSpan / DateOnly / TimeOnly
// / Guid, and `a.CompareTo(b)` through `T: IComparable<T>` to `0` for Guid
// (behind an `internal:` degrade warning). The codegen now resolves the
// substitution for those receivers (the Guid arm in `tryLowerBuiltinClassCall`;
// the #1091 substitution arm of `tryLowerValueSemanticsCall` for the three
// scalar value records), and ROWS 5-6 pin the computed values.
//
// ORACLE. `runAot` — the AOT path, the strict oracle. Every ACCEPTING row
// asserts the EXACT STDOUT (a VALUE oracle, never "no diagnostic was
// produced"); every REFUSING row asserts the SPECIFIC diagnostic text and
// that the refusal is a COMPILE-TIME refusal at a REAL location.
//
// THE TWO CONTROLS (green before the fix too; the owner's cap is two per
// suite) each name the plausible WRONG FIX they catch: a USER type named
// `TimeSpan` is still refused (catches recognizing the builtin by NAME), and
// a bound naming a DIFFERENT instantiation (`IComparable<DateTime>`) is still
// refused (catches ignoring the bound's type argument). The other
// spec-decided, base-green shapes (a user record declaring `IComparable<Rec>`
// is admitted; one declaring none is refused; `IEqualityComparer<T>` is
// refused for a `TimeSpan`) are conformance-corpus candidates, not gtest rows.

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

// THE MEMBERS THROUGH T. The admission is half of §14.2; the other half is
// "Inside the generic body, T's surface includes IFoo's members" — so an
// admitted `T` must answer `a.Equals(b)` / `a.CompareTo(b)` with its OWN
// value's answer. `lo` and `hi` differ; `hi2` is a SEPARATELY constructed
// value equal to `hi`, so `Eq(hi, hi2)` reads the value, not an identity.
// Each line prints `Eq(lo, hi) Eq(hi, hi) Eq(hi, hi2)` = "False True True".
std::string equalsThroughTProgram(const std::string& decls) {
    return "module Conf;\n"
           "public class Program\n"
           "  public static method Eq<T>(a: T, b: T): bool where T: IEquatable<T>\n"
           "    return a.Equals(b);\n"
           "  end Eq;\n"
           "  public static method Main(): int\n"
           "    " + decls + "\n"
           "    Console.WriteLine($\"{Eq(lo, hi)} {Eq(hi, hi)} {Eq(hi, hi2)}\");\n"
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

// `CompareTo` through T for a `Guid`: stdlib §2.6 orders a Guid
// "lexicographic over the 16 canonical big-endian bytes", so `lo` (leading
// byte 0x00) sorts before `hi` (leading byte 0xff). The signs are printed
// (stdlib §2.6 does not fix the magnitude), plus the generic answer against
// the DIRECT `lo.CompareTo(hi)` — the two spellings of one member must agree.
const char* kGuidCompareToThroughT =
    "module Conf;\n"
    "public class Program\n"
    "  public static method Cmp<T>(a: T, b: T): int where T: IComparable<T>\n"
    "    return a.CompareTo(b);\n"
    "  end Cmp;\n"
    "  public static method Main(): int\n"
    "    var lo := Guid.Parse(\"00000000-0000-0000-0000-000000000001\");\n"
    "    var hi := Guid.Parse(\"ffffffff-0000-0000-0000-000000000002\");\n"
    "    Console.WriteLine($\"{Cmp(lo, hi) < 0} {Cmp(hi, lo) > 0} "
    "{Cmp(lo, lo) = 0} {Cmp(lo, hi) = lo.CompareTo(hi)}\");\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// `GetHashCode` through an UNCONSTRAINED T (the §8.1 universal Object
// contract every type carries). spec §8.3 (v0.357): "if `a.Equals(b)` then
// `a.GetHashCode() = b.GetHashCode()`" — so the generic answer must equal the
// DIRECT `x.GetHashCode()` of the same value, and an equal, separately
// constructed `hi2` must hash like `hi`. Each line prints "True True True".
std::string hashThroughTProgram(const std::string& decls) {
    return "module Conf;\n"
           "public class Program\n"
           "  public static method H<T>(a: T): int\n"
           "    return a.GetHashCode();\n"
           "  end H;\n"
           "  public static method Main(): int\n"
           "    " + decls + "\n"
           "    Console.WriteLine($\"{H(lo) = lo.GetHashCode()} "
           "{H(hi) = hi.GetHashCode()} {H(hi) = H(hi2)}\");\n"
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

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
// ROW 5 — `a.Equals(b)` THROUGH `T: IEquatable<T>` computes each value
// builtin's own §8.3 value equality. The type is the ROW'S DATA.
//
// Pre-fix (the admission alone, c48f3821b): TimeSpan / DateOnly / TimeOnly /
// Guid printed "False False False" behind "internal: return-value expression
// failed to lower to a value (degraded to a zero/null return value)" — a
// silent wrong answer for a program the compiler admits. DateTime was already
// right (its own arm resolves the substitution, #7765 / #8064) and is kept in
// the row so a fix that breaks it fails here too.
// ---------------------------------------------------------------------------
BEZEL_TEST(value_builtin_generic_constraint_8578,
           equals_through_t_is_each_value_builtins_own_equality) {
    BEZEL_EXPECT_EQ(
        out(equalsThroughTProgram(
            "var lo := TimeSpan.FromTicks(5L); "
            "var hi := TimeSpan.FromTicks(900L); "
            "var hi2 := TimeSpan.FromTicks(900L);")),
        "False True True\n");
    BEZEL_EXPECT_EQ(
        out(equalsThroughTProgram(
            "var lo := new DateOnly(2020, 1, 1); "
            "var hi := new DateOnly(2021, 5, 6); "
            "var hi2 := new DateOnly(2021, 5, 6);")),
        "False True True\n");
    BEZEL_EXPECT_EQ(
        out(equalsThroughTProgram(
            "var lo := new TimeOnly(1, 2, 3); "
            "var hi := new TimeOnly(10, 20, 30); "
            "var hi2 := new TimeOnly(10, 20, 30);")),
        "False True True\n");
    BEZEL_EXPECT_EQ(
        out(equalsThroughTProgram(
            "var lo := Guid.Parse(\"00000000-0000-0000-0000-000000000001\"); "
            "var hi := Guid.Parse(\"ffffffff-0000-0000-0000-000000000002\"); "
            "var hi2 := Guid.Parse(\"ffffffff-0000-0000-0000-000000000002\");")),
        "False True True\n");
    BEZEL_EXPECT_EQ(
        out(equalsThroughTProgram(
            "var lo := new DateTime(2020, 1, 1); "
            "var hi := new DateTime(2021, 5, 6); "
            "var hi2 := new DateTime(2021, 5, 6);")),
        "False True True\n");
}

// ---------------------------------------------------------------------------
// ROW 6 — `a.CompareTo(b)` THROUGH `T: IComparable<T>` at `T = Guid` orders
// by the Guid's bytes (stdlib §2.6).
//
// Pre-fix (c48f3821b): every comparison returned 0 behind the same
// `internal:` degrade warning — "False False True False".
// ---------------------------------------------------------------------------
BEZEL_TEST(value_builtin_generic_constraint_8578,
           compare_to_through_t_orders_a_guid_by_its_bytes) {
    BEZEL_EXPECT_EQ(out(kGuidCompareToThroughT), "True True True True\n");
}

// ---------------------------------------------------------------------------
// ROW 7 — `a.GetHashCode()` THROUGH `T` computes the value builtin's own hash
// (the same lowering ROW 5's `Equals` now takes, so the §8.3 pairing
// "if `a.Equals(b)` then `a.GetHashCode() = b.GetHashCode()`" holds through T).
//
// Pre-fix: refused at compile time for TimeSpan / DateOnly / TimeOnly / Guid —
// "member `GetHashCode` on `T` is declared via the universal Object contract
// (language spec §8.1) but is not yet implemented by this compiler for this
// receiver ... (tracked #5953)". The program never ran.
// ---------------------------------------------------------------------------
BEZEL_TEST(value_builtin_generic_constraint_8578,
           get_hash_code_through_t_is_each_value_builtins_own_hash) {
    BEZEL_EXPECT_EQ(
        out(hashThroughTProgram(
            "var lo := TimeSpan.FromTicks(5L); "
            "var hi := TimeSpan.FromTicks(900L); "
            "var hi2 := TimeSpan.FromTicks(900L);")),
        "True True True\n");
    BEZEL_EXPECT_EQ(
        out(hashThroughTProgram(
            "var lo := new DateOnly(2020, 1, 1); "
            "var hi := new DateOnly(2021, 5, 6); "
            "var hi2 := new DateOnly(2021, 5, 6);")),
        "True True True\n");
    BEZEL_EXPECT_EQ(
        out(hashThroughTProgram(
            "var lo := new TimeOnly(1, 2, 3); "
            "var hi := new TimeOnly(10, 20, 30); "
            "var hi2 := new TimeOnly(10, 20, 30);")),
        "True True True\n");
    BEZEL_EXPECT_EQ(
        out(hashThroughTProgram(
            "var lo := Guid.Parse(\"00000000-0000-0000-0000-000000000001\"); "
            "var hi := Guid.Parse(\"ffffffff-0000-0000-0000-000000000002\"); "
            "var hi2 := Guid.Parse(\"ffffffff-0000-0000-0000-000000000002\");")),
        "True True True\n");
}

// ---------------------------------------------------------------------------
// CONTROL 1 of 2 (green before the fix too) — a USER type named `TimeSpan`
// (shadowing the builtin) declares NO `IComparable` and is still REFUSED.
//
// THE WRONG FIX IT CATCHES: recognizing the stdlib value builtin by its
// simple NAME (`builtinTypeName(...) == "TimeSpan"`) instead of by the
// intrinsic symbol's shape (`isValueBuiltin`, no `decl`, no `moduleName`).
// That fix passes every row above and admits this user record, whose
// declaration lists no `IComparable` (§14.2 rule 2 does not hold).
// ---------------------------------------------------------------------------
BEZEL_TEST(value_builtin_generic_constraint_8578,
           user_type_named_timespan_is_still_refused) {
    JitResult r = runAot(kUserTypeNamedTimeSpan);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kConstraintRefusal));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// ---------------------------------------------------------------------------
// CONTROL 2 of 2 (green before the fix too) — a bound naming a DIFFERENT
// instantiation (`IComparable<DateTime>`) is still REFUSED. `TimeSpan`
// conforms to `IComparable<TimeSpan>` (stdlib §2.5), not to
// `IComparable<DateTime>`.
//
// THE WRONG FIX IT CATCHES: admitting a value builtin against ANY
// `IComparable<…>` / `IEquatable<…>` head without checking the bound's type
// argument names the candidate. That fix passes every row above (they all
// use `IComparable<T>` or `IComparable<TimeSpan>` for a `TimeSpan`).
// ---------------------------------------------------------------------------
BEZEL_TEST(value_builtin_generic_constraint_8578,
           a_different_icomparable_instantiation_is_still_refused) {
    JitResult r = runAot(kWrongInstantiation);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kConstraintRefusal));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}
