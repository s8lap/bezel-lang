// fix #8591 (spec §14.2 *Constraints* — the "Type-parameter bounds" table,
// stdlib §2.5/§2.6, v0.357): a `where T: class` bound ADMITS a stdlib value
// builtin (TimeSpan / DateTime / DateOnly / TimeOnly / Guid).
//
// SPEC BASIS (QUOTED, v0.357 — spec §14.2, the "Type-parameter bounds" table):
//   "where T: class" | "Non-nullable reference types only — classes (including
//    `Array<T>`, `OwningArray<T>`, and other heap collection types),
//    interfaces, `string`, function types."
//   "where T: value" | "Non-nullable value types — records, enums, choices
//    (§35), primitives, inline arrays (all forms), the unit type. A nullable
//    value type (e.g. `int?`) does **not** satisfy `value`."
// and §7.10 (v0.357): "`where T: class` restricts T to non-nullable reference
// types — classes (including `Array<T>`, `OwningArray<T>`), interfaces,
// `string`, function types."
//
// The stdlib value builtins are RECORDS — stdlib §2.5: "public record
// DateTime: IComparable<DateTime>, IEquatable<DateTime>" and "public record
// TimeSpan: IComparable<TimeSpan>, IEquatable<TimeSpan>"; stdlib §2.6: "public
// record Guid: IComparable<Guid>, IEquatable<Guid>" — so they are VALUE types
// under §14.2's `value` row and NOT reference types: a `where T: class` bound
// must REFUSE them.
//
// BEFORE THE FIX: the compiler-intrinsic symbol the compiler mints for those
// names (`SemaContext::builtinClassSym`) carries `kind = Class` — a
// deliberate simplification ("They carry kind==Class so that the sema and
// codegen paths that inspect class-typed member accesses continue to work
// unchanged") — so both bound-site classifiers
// (`Resolver::categorize` and the call-site `refKindOf` mirror) read the
// value builtin as a REFERENCE type and the `class` bound admits it.
//
// Every row below FAILS without the fix (the base `r30-c1` @ e69d8b05
// COMPILES each program and prints "accepted").

#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

// The house §14.2 constraint diagnostic, spelled once.
constexpr const char* kClassBoundViolation =
    "does not satisfy the constraint `where T: class` (\xC2\xA7" "14.2)";

}  // namespace

// ---------------------------------------------------------------------------
// R1 — THE ISSUE'S REPRO (verbatim): `F(TimeSpan.FromTicks(5L))` at
// `where T: class`. Before the fix: COMPILES, prints `accepted`.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_class_bound_value_builtin_8591,
           issue_repro_class_bound_refuses_a_stdlib_value_builtin) {
    JitResult r = runAot(R"(
module Conf;
public class Program
  public static method Main(): int
    return F(TimeSpan.FromTicks(5L));
  end Main;
  public static method F<T>(x: T): int where T: class
    Console.WriteLine("accepted");
    return 0;
  end F;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `TimeSpan` "));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kClassBoundViolation));
}

// ---------------------------------------------------------------------------
// R2 — the TYPE-INSTANTIATION site: `class Box<T> where T: class` with
// `new Box<TimeSpan>(...)`. Before the fix: COMPILES, prints `accepted`.
//
// This is the §14.2 check at a type instantiation rather than at a call, so
// it exercises the OTHER classifier of the two (`Resolver::categorize`, at
// `Resolver::checkOneConstraint`) — the two bounds' sites are separate
// implementations and both must refuse.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_class_bound_value_builtin_8591,
           type_instantiation_site_class_bound_refuses_a_stdlib_value_builtin) {
    JitResult r = runAot(R"(
module Conf;
public class Box<T> where T: class
  public readonly property V: T;
  public constructor(v: T)
    V := v;
  end constructor;
end Box;
public class Program
  public static method Main(): int
    var b := new Box<TimeSpan>(TimeSpan.FromTicks(5L));
    Console.WriteLine("accepted");
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `TimeSpan` "));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kClassBoundViolation));
}

// ---------------------------------------------------------------------------
// R3 — DateTime (stdlib §2.5) at `where T: class`. Before the fix: COMPILES.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_class_bound_value_builtin_8591,
           class_bound_refuses_datetime) {
    JitResult r = runAot(R"(
module Conf;
public class Program
  public static method Main(): int
    return F(new DateTime(2020, 1, 1));
  end Main;
  public static method F<T>(x: T): int where T: class
    Console.WriteLine("accepted");
    return 0;
  end F;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `DateTime` "));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kClassBoundViolation));
}

// ---------------------------------------------------------------------------
// R4 — DateOnly (stdlib §2.5) at `where T: class`. Before the fix: COMPILES.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_class_bound_value_builtin_8591,
           class_bound_refuses_dateonly) {
    JitResult r = runAot(R"(
module Conf;
public class Program
  public static method Main(): int
    return F(new DateOnly(2020, 1, 1));
  end Main;
  public static method F<T>(x: T): int where T: class
    Console.WriteLine("accepted");
    return 0;
  end F;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `DateOnly` "));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kClassBoundViolation));
}

// ---------------------------------------------------------------------------
// R5 — TimeOnly (stdlib §2.5) at `where T: class`. Before the fix: COMPILES.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_class_bound_value_builtin_8591,
           class_bound_refuses_timeonly) {
    JitResult r = runAot(R"(
module Conf;
public class Program
  public static method Main(): int
    return F(new TimeOnly(1, 2, 3));
  end Main;
  public static method F<T>(x: T): int where T: class
    Console.WriteLine("accepted");
    return 0;
  end F;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `TimeOnly` "));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kClassBoundViolation));
}

// ---------------------------------------------------------------------------
// R6 — Guid (stdlib §2.6) at `where T: class`. Before the fix: COMPILES.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_class_bound_value_builtin_8591,
           class_bound_refuses_guid) {
    JitResult r = runAot(R"(
module Conf;
public class Program
  public static method Main(): int
    return F(Guid.NewGuid());
  end Main;
  public static method F<T>(x: T): int where T: class
    Console.WriteLine("accepted");
    return 0;
  end F;
end Program;
)");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "type argument `Guid` "));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kClassBoundViolation));
}
