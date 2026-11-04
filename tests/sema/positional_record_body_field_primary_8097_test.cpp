// Regression tests for #8097: a POSITIONAL record's non-defaultable body
// field / auto-property with NO initializer is accepted today when the body
// declares a constructor, and must be REFUSED.
//
// Spec §9.3 (Combined form), v0.355, verbatim (docs/Bezel_Language_Spec.md
// line 1424):
//
//   "The auto-generated primary assigns the positional storage only, and
//    §7.9's obligation is per constructor, so a non-defaultable body field or
//    auto-property is satisfied only by a field initializer (§22.8); a body
//    constructor, of any parameter list, does not assign it for the primary."
//
// Spec §7.9 (Definite assignment for non-nullable fields), v0.355, verbatim
// (line 1088):
//
//   "Every non-nullable field must be either:
//    - assigned a value in every constructor before the constructor returns, or
//    - given a field initializer at its declaration."
//
// and §7.9's "Default initialization" subsection (line 1099):
//
//   "A field whose type is defaultable (§17.11 — every nullable type, whose
//    default is `null`, and every value type the property admits) satisfies the
//    must-initialize rule vacuously and is implicitly initialized to that
//    default; an explicit initializer is permitted and overrides it. A field
//    whose type is not defaultable — a non-nullable reference type, a choice,
//    or a value type with a non-defaultable component — requires an explicit
//    initializer: a field initializer, or assignment in every constructor (for
//    instance fields) or in the static constructor (for static fields). The
//    compiler emits a definite-assignment error if neither path supplies a
//    value."
//
// §17.11 names `string` among the NON-defaultable reference types ("a
// non-nullable reference type (a class, an interface, `Object`, `string`, a
// function type, a task type)"), and `int` among the defaultable primitives —
// so the ACCEPT rows below use `int` for the defaultable case and `string`
// for the non-defaultable one.
//
// THE DEFECT: `checkRecordNoCtorFieldCompletion` (src/sema/DefiniteAssignment.cpp)
// returned early for ANY record whose body declared an instance constructor.
// For a POSITIONAL record that is wrong: the §9.1 auto-generated primary
// exists alongside the body constructor, it assigns the positional storage
// only, and §7.9's obligation is PER CONSTRUCTOR — so `new R(9)` (the
// primary) left the body field at its zero-filled slot with no diagnostic.
// The BODY-record shape (no positional list) has NO auto-generated primary, so
// its declared constructor carries the whole obligation and that shape is
// deliberately unchanged; the ACCEPT rows below pin it.
//
// Every REJECT row uses `failedAtCompileTimeWithRealLocation` (not a bare
// `!r.ran`, which a compiler crash / signal / timeout would satisfy too) and
// pins the diagnostic TEXT — the field name, and the §9.3 / §7.9 citations —
// so a diagnostic-text rename is caught here. Every ACCEPT row runs on the AOT
// oracle (`runAot`, the strict one) and compares EXACT stdout, so a false
// reject shows as a value mismatch rather than passing silently.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The class the repro's non-defaultable body field has its type: a
// non-nullable reference type, so §7.9's must-initialize rule applies
// (not defaultable per §17.11).
const char* kCell =
    "public class Cell8097 "
    "  public V: int := 0; "
    "  public constructor() V := 7; end constructor; "
    "end Cell8097; ";

const char* kMain =
    "public class Program8097 "
    "  public static method Main() "
    "    Console.WriteLine(1); "
    "  end Main; "
    "end Program8097;";

}  // namespace

// =====================================================================
// REJECT arm 1 — the issue's own repro: a POSITIONAL record whose body
// declares a non-defaultable field with NO initializer, plus a body
// constructor of a different parameter list that DOES assign it. The
// assignment discharges the BODY constructor's own §7.9 obligation, never
// the primary's (§9.3).
// =====================================================================
BEZEL_TEST(sema_positional_record_body_field_primary_8097,
           body_field_without_initializer_rejected_spec_9_3) {
    auto r = runAot(std::string("module Conf; ") + kCell +
        "public record R8097(seed: int) "
        "  private _c: Cell8097; "
        "  public constructor(seed: int, c: Cell8097) "
        "    this.seed := seed; "
        "    _c := c; "
        "  end constructor; "
        "end R8097; " + kMain);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `_c`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "\xC2\xA7""9.3"));
    BEZEL_EXPECT_TRUE(diagContains(r, "\xC2\xA7""7.9"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// =====================================================================
// REJECT arm 2 — the same shape with a `readonly property` AUTO-PROPERTY in
// place of the explicit field. §9.3 names "a non-defaultable body field OR
// AUTO-PROPERTY", and §22.7 makes the auto-property's backing storage carry
// the same obligation.
// =====================================================================
BEZEL_TEST(sema_positional_record_body_field_primary_8097,
           body_auto_property_without_initializer_rejected_spec_9_3) {
    auto r = runAot(std::string("module Conf; ") + kCell +
        "public record R8097(seed: int) "
        "  public readonly property C: Cell8097; "
        "  public constructor(seed: int, c: Cell8097) "
        "    this.seed := seed; "
        "    C := c; "
        "  end constructor; "
        "end R8097; " + kMain);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `C`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "\xC2\xA7""9.3"));
    BEZEL_EXPECT_TRUE(diagContains(r, "\xC2\xA7""7.9"));
}

// =====================================================================
// REJECT arm 3 — a GENERIC positional record: the primary's obligation is
// the same at the generic declaration, so the shape is refused there too
// (not deferred to an instantiation).
// =====================================================================
BEZEL_TEST(sema_positional_record_body_field_primary_8097,
           generic_positional_body_field_without_initializer_rejected_spec_9_3) {
    auto r = runAot(std::string("module Conf; ") + kCell +
        "public record R8097<T>(seed: T) "
        "  private _c: Cell8097; "
        "  public constructor(seed: T, c: Cell8097) "
        "    this.seed := seed; "
        "    _c := c; "
        "  end constructor; "
        "end R8097; " + kMain);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `_c`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "\xC2\xA7""9.3"));
}

// =====================================================================
// REJECT arm 4 — the body constructor CHAINS to the primary (`this(seed)`),
// so the primary's own path runs first and still leaves the body field
// unset. §9.3's sentence is about the primary, not about the chain.
// =====================================================================
BEZEL_TEST(sema_positional_record_body_field_primary_8097,
           chained_body_constructor_does_not_satisfy_primary_spec_9_3) {
    auto r = runAot(std::string("module Conf; ") + kCell +
        "public record R8097(seed: int) "
        "  private _c: Cell8097; "
        "  public constructor(seed: int, c: Cell8097) "
        "    this(seed); "
        "    _c := c; "
        "  end constructor; "
        "end R8097; " + kMain);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `_c`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "\xC2\xA7""9.3"));
}

// =====================================================================
// REJECT arm 5 — TWO body constructors, one of which assigns the field and
// one of which does not. Neither discharges the PRIMARY's obligation.
// =====================================================================
BEZEL_TEST(sema_positional_record_body_field_primary_8097,
           one_of_two_body_constructors_rejected_spec_9_3) {
    auto r = runAot(std::string("module Conf; ") + kCell +
        "public record R8097(seed: int) "
        "  private _c: Cell8097; "
        "  public constructor(seed: int, c: Cell8097) "
        "    this.seed := seed; "
        "    _c := c; "
        "  end constructor; "
        "  public constructor(seed: int, c: Cell8097, extra: int) "
        "    this.seed := seed; "
        "    _c := c; "
        "  end constructor; "
        "end R8097; " + kMain);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `_c`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "\xC2\xA7""9.3"));
}

// =====================================================================
// REJECT arm 6 — a `string` body field with NO initializer. §17.11 names
// `string` among the NON-defaultable reference types, so the must-initialize
// rule applies to it exactly as it applies to a class-typed field.
// =====================================================================
BEZEL_TEST(sema_positional_record_body_field_primary_8097,
           string_body_field_without_initializer_rejected_spec_9_3) {
    auto r = runAot(std::string("module Conf; ") +
        "public record R8097(seed: int) "
        "  public S: string; "
        "  public constructor(seed: int, s: string) "
        "    this.seed := seed; "
        "    S := s; "
        "  end constructor; "
        "end R8097; " + kMain);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `S`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "\xC2\xA7""9.3"));
    BEZEL_EXPECT_TRUE(diagContains(r, "\xC2\xA7""7.9"));
}

// =====================================================================
// REJECT arm 7 (CONTROL, unchanged by this fix) — a POSITIONAL record with a
// non-defaultable body field and NO constructor at all. The pre-existing
// no-ctor screen still fires, with ITS OWN diagnostic text (which names
// "§9.1" and the absence of a constructor). Pinned so the new §9.3 message
// cannot silently replace it.
// =====================================================================
BEZEL_TEST(sema_positional_record_body_field_primary_8097,
           no_constructor_at_all_still_rejected_spec_7_9) {
    auto r = runAot(std::string("module Conf; ") + kCell +
        "public record R8097(seed: int) "
        "  private _c: Cell8097; "
        "end R8097; " + kMain);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "non-nullable field `_c`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "this record declares no constructor"));
    BEZEL_EXPECT_TRUE(diagContains(r, "\xC2\xA7""9.1"));
}

// =====================================================================
// ACCEPT arm 1 — a DEFAULTABLE body field (`int`, a primitive, so
// defaultable per §17.11) with NO initializer, beside a body constructor.
// §7.9's must-initialize rule "satisfies the must-initialize rule
// vacuously" for a defaultable field, so this stays accepted and the
// program RUNS. Its exact stdout is the oracle.
// =====================================================================
BEZEL_TEST(sema_positional_record_body_field_primary_8097,
           defaultable_body_field_no_initializer_accepted_spec_7_9) {
    auto r = runAot(std::string("module Conf; ") +
        "public record R8097(seed: int) "
        "  public N: int; "
        "  public constructor(seed: int, n: int) "
        "    this.seed := seed; "
        "    N := n; "
        "  end constructor; "
        "end R8097; "
        "public class Program8097 "
        "  public static method Main() "
        "    var r := new R8097(9, 3); "
        "    Console.WriteLine(r.seed + r.N); "
        "  end Main; "
        "end Program8097;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("12\n"));
}

// =====================================================================
// ACCEPT arm 2 — a NULLABLE body field (`string?`) with no initializer:
// every nullable type is defaultable (§17.11), so the rule is vacuous and
// the shape stays accepted.
// =====================================================================
BEZEL_TEST(sema_positional_record_body_field_primary_8097,
           nullable_body_field_no_initializer_accepted_spec_7_9) {
    auto r = runAot(std::string("module Conf; ") +
        "public record R8097(seed: int) "
        "  public S: string?; "
        "  public constructor(seed: int, s: string?) "
        "    this.seed := seed; "
        "    S := s; "
        "  end constructor; "
        "end R8097; "
        "public class Program8097 "
        "  public static method Main() "
        "    var r := new R8097(9, null); "
        "    Console.WriteLine((if r.S = null then 0 else 1)); "
        "  end Main; "
        "end Program8097;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// =====================================================================
// ACCEPT arm 3 — a NON-defaultable body field WITH a field initializer
// (§22.8). §9.3: "a non-defaultable body field or auto-property is
// satisfied only by a field initializer (§22.8)". The field initializer
// discharges the obligation for EVERY constructor, the primary included.
// =====================================================================
BEZEL_TEST(sema_positional_record_body_field_primary_8097,
           non_defaultable_body_field_with_initializer_accepted_spec_22_8) {
    auto r = runAot(std::string("module Conf; ") +
        "public record R8097(seed: int) "
        "  public S: string := \"hi\"; "
        "  public constructor(seed: int, s: string) "
        "    this.seed := seed; "
        "    S := s; "
        "  end constructor; "
        "end R8097; "
        "public class Program8097 "
        "  public static method Main() "
        "    var r := new R8097(9, \"yo\"); "
        "    Console.WriteLine(r.S); "
        "  end Main; "
        "end Program8097;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("yo\n"));
}

// =====================================================================
// ACCEPT arm 4 — the BODY-record twin: a record with NO positional list has
// NO auto-generated primary, so its declared constructor carries the whole
// §7.9 obligation and assigns the field. This shape is deliberately
// UNCHANGED by the fix (§9.3 speaks only of the auto-generated primary), and
// it is the legal form the #8089 fix-forward (5c1b2f27) moved its
// private-backing fixtures to.
// =====================================================================
BEZEL_TEST(sema_positional_record_body_field_primary_8097,
           body_record_with_constructor_assigning_field_accepted_spec_9_2) {
    auto r = runAot(std::string("module Conf; ") + kCell +
        "public record R8097 "
        "  public readonly property Seed: int; "
        "  private _c: Cell8097; "
        "  public constructor(seed: int, c: Cell8097) "
        "    Seed := seed; "
        "    _c := c; "
        "  end constructor; "
        "end R8097; "
        "public class Program8097 "
        "  public static method Main() "
        "    using var c := new Cell8097(); "
        "    var r := new R8097(9, c); "
        "    Console.WriteLine(r.Seed); "
        "  end Main; "
        "end Program8097;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9\n"));
}

// =====================================================================
// ACCEPT arm 5 — a POSITIONAL record with NO body fields at all: the
// primary's obligation is empty, so nothing is refused.
// =====================================================================
BEZEL_TEST(sema_positional_record_body_field_primary_8097,
           positional_record_without_body_fields_accepted_spec_9_1) {
    auto r = runAot(std::string("module Conf; ") +
        "public record R8097(seed: int) "
        "end R8097; "
        "public class Program8097 "
        "  public static method Main() "
        "    var r := new R8097(9); "
        "    Console.WriteLine(r.seed); "
        "  end Main; "
        "end Program8097;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9\n"));
}

// =====================================================================
// ACCEPT arm 6 — a GENERIC positional record whose body field is
// DEFAULTABLE: legal at the generic declaration and at the instantiation.
// The generic twin of REJECT arm 3.
// =====================================================================
BEZEL_TEST(sema_positional_record_body_field_primary_8097,
           generic_positional_record_defaultable_body_field_accepted) {
    auto r = runAot(std::string("module Conf; ") +
        "public record R8097<T>(seed: T) "
        "  public N: int; "
        "  public constructor(seed: T, n: int) "
        "    this.seed := seed; "
        "    N := n; "
        "  end constructor; "
        "end R8097; "
        "public class Program8097 "
        "  public static method Main() "
        "    var r := new R8097<int>(9, 4); "
        "    Console.WriteLine(r.seed + r.N); "
        "  end Main; "
        "end Program8097;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("13\n"));
}
