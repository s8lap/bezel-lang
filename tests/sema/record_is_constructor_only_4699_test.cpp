// fix #4699 (§9.9 "Constructor-only records", v0.307, proposal 0107): a
// record is **constructor-only** when it declares a constructor carrying a
// contract clause (§22.16 — a `requires` clause, an `ensures` clause, or
// both). The property triggers three downstream consequences in this
// revision:
//
//   (a) `default(R)` is rejected (§17.11's "Not defaultable" list names it
//       directly).
//   (b) `where T: new()` at a substitution to such a `R` is rejected
//       (§14.2's "value substitution is equivalent to `default(T)`" rule
//       collapses to (a)).
//   (c) default initialization of `R`-typed storage is rejected (§7.9 /
//       §15.1's "every constructor must assign every non-defaultable field"
//       obligation; the constructor-only property is the missing source of
//       "non-defaultable" — the fields can all be defaultable value types,
//       but the record's own property still forbids the zero value).
//
// SHIP-SHAPE SWEEP (each test below is the spec's exact repro — assert the
// full compiled program against its expected outcome, never a coincidental
// pass):
//
//   - REJECT (a): `default(Port)` where Port is a body record with a
//       requires-carrying ctor. Compiles before the fix; the rejection
//       names the constructor-only record.
//   - REJECT (b): `Factory.Make<Port>()` against `where T: new()`. Compiles
//       before the fix; the rejection names the constructor-only record.
//   - REJECT (c): `private _port: Port;` with no initializer. Compiles
//       before the fix; the rejection names the §9.9 reason.
//   - ACCEPT controls (3): a record with NO clause-carrying constructor
//       stays defaultable; a record with a clause-bearing constructor whose
//       field is INITIALIZED stays accepted at every site; a constructor-
//       only record still composes with `=` equality (§9.2's membership
//       rule keeps auto-hashable alive).
//
// IMPLEMENTATION: a single `recordIsConstructorOnly(const TypeSymbol*)`
// predicate in sema/Symbol.hpp (definition in SymbolCollection.cpp) —
// walks the AST `TypeDecl::members` for any `ConstructorDecl` whose
// `contractClauses` is non-empty, returning false for an imported record
// (cross-module AST unavailable; the wire-channel infrastructure for the
// §2.8.2 carriage is not yet built — issue #3301's broader impl-catch-up
// tracks it). The predicate is consumed at three sites:
//
//   - `defaultIllFormed` → `typeHasNoDefault` short-circuit
//     (TypeResolution_Checks.cpp): the §17.11 default-operator well-formedness
//     check, with a new constructor-only-specific diagnostic branch in
//     `ExprTC::defaultIllFormed`'s dispatch (ExprTypecheck_Exprs.cpp).
//   - `Resolver::argSatisfiesNew` Record arm (TypeResolution_Checks.cpp):
//     the §14.2 `where T: new()` declaration-site check.
//   - `ExprTC::groundSatisfiesNew` Record arm (ExprTypecheck.cpp): the
//     ExprTypecheck sibling of argSatisfiesNew.
//   - `checkMethodGenericConstraints` Record arm (ExprTypecheck_Calls.cpp):
//     the §14.2 call-site re-check that was previously a bare deferral
//     and dropped constructor-only records on the floor.
//   - `isSevenNineStrictField` / `isSevenNineStrictAutoProp` /
//     `isSevenNineStrictStaticField` / `isSevenNineStrictStaticAutoProp`
//     (DefiniteAssignment.cpp): the §7.9 field-DA classification; a
//     constructor-only-typed field/auto-prop joins the needing-set, with
//     `checkCtorFieldCompletion` extending the §7.9 diagnostic to name
//     the §9.9 reason.
//
// FILTERS RUN (no `--area`): `9_9` / `default_t` / `17_11` / `where_t_new`
// / `record_field` / `7_9`. No pre-existing test was relying on the
// pre-fix (incorrect) admission — measured zero false-reject cost.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// A constructor-only record: body form, one readonly auto-property, a single
// requires-clause-carrying ctor. Self-contained — kept private to this file
// so the test cases read declaratively.
const char* kPort =
    "record Port "
    "  public readonly property Value: int; "
    "  public constructor(value: int) "
    "    requires(value >= 0 and value <= 65535) "
    "    Value := value; "
    "  end constructor; "
    "end Port; ";

// Asserts a clean, real-location §17.11 rejection of `default(Port)`, with
// the diagnostic naming the §9.9 "constructor-only" property.
void expectDefaultPortRejected(const std::string& program) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "constructor-only"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""9.9"));
}

// Asserts a clean, real-location §14.2 rejection of a `where T: new()`
// substitution with a constructor-only Port, with the diagnostic naming
// the §9.9 reason (the property makes the substitution unsatisfiable).
void expectWhereNewPortRejected(const std::string& program) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "new()"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "constructor-only"));
}

// Asserts a clean, real-location §7.9 rejection of a constructor-only-typed
// field that lacks an initializer, with the diagnostic naming the §9.9
// reason (the property is what makes the field §7.9-strict, not any
// non-defaultable field type).
void expectDefaultInitPortFieldRejected(const std::string& program) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "constructor-only"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""7.9"));
}

}  // namespace

// =====================================================================
// fix #4699 / §9.9 / §17.11 — REJECT (a): `default(R)` on a constructor-
// only record is rejected
// =====================================================================

// The spec's canonical repro (s9_9_0002 / s17_11_0010): `default(Port)` where
// Port is a body record with a requires-carrying ctor. Pre-fix this compiled
// clean (the per-field `defaultIllFormed` walk saw only `int Value` and
// returned "defaultable"); post-fix it rejects and names the constructor-
// only property.
BEZEL_TEST(record_is_constructor_only_4699,
           default_of_constructor_only_record_rejected_spec_9_9_17_11) {
    expectDefaultPortRejected(
        std::string("module M; ") + kPort +
        "class Program "
        "  public static method Main(): int "
        "    var p := default(Port); "
        "    return p.Value; "
        "  end Main; "
        "end Program;");
}

// =====================================================================
// fix #4699 / §9.9 / §14.2 — REJECT (b): `where T: new()` with a
// constructor-only substitution is rejected
// =====================================================================

// The spec's canonical repro (s9_9_0003): a static factory constrained
// `where T: new()` instantiated with Port. Pre-fix the ExprTypecheck call-
// site Record arm deferred and the declaration-site check never fired for
// this shape; post-fix both arms reject and name the constructor-only
// reason. The diagnostic location is the substitution site
// (`Factory.Make<Port>()`), per §3.6 rule 4.
BEZEL_TEST(record_is_constructor_only_4699,
           where_new_constraint_rejects_constructor_only_record_spec_9_9_14_2) {
    expectWhereNewPortRejected(
        std::string("module M; ") + kPort +
        "class Factory "
        "  public static method Make<T>(): owned T where T: new() => new T(); "
        "end Factory; "
        "class Program "
        "  public static method Main(): int "
        "    var p := Factory.Make<Port>(); "
        "    return p.Value; "
        "  end Main; "
        "end Program;");
}

// =====================================================================
// fix #4699 / §9.9 / §7.9 — REJECT (c): default initialization of
// constructor-only-typed storage is rejected
// =====================================================================

// The spec's canonical repro (s9_9_0006): a private Port-typed field with
// no initializer and a parameterless constructor. Pre-fix `Port`'s only
// storage (`int Value`) is defaultable, so `isSevenNineStrictField`
// returned false; post-fix the field joins the needing-set and the
// §7.9 diagnostic names the §9.9 reason.
BEZEL_TEST(record_is_constructor_only_4699,
           default_init_of_constructor_only_storage_rejected_spec_9_9_7_9) {
    expectDefaultInitPortFieldRejected(
        std::string("module M; ") + kPort +
        "class Listener "
        "  private _port: Port; "
        "  public constructor() end constructor; "
        "end Listener; "
        "class Program "
        "  public static method Main(): int "
        "    using var l := new Listener(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
}

// =====================================================================
// ACCEPT CONTROLS — over-rejection guards
// =====================================================================

// A body record with NO clause-bearing ctor is ordinary — `default(R)`,
// `where T: new()`, and default initialization all stay accepted (the
// §9.9 property is the trigger; an ordinary body record has no clause to
// carry). Mirrors the spec's negative control at s9_9_0004 (the
// `Coordinate` twin). Pre-fix and post-fix the program compiles AND runs
// — value-bearing readback so a false reject shows as compile failure.
BEZEL_TEST(record_is_constructor_only_4699,
           ordinary_body_record_no_clause_stays_defaultable_positive_control) {
    auto r = runAot(
        "module M; "
        "record Ordinary "
        "  public readonly property Value: int; "
        "  public constructor(value: int) "
        "    Value := value; "
        "  end constructor; "
        "end Ordinary; "
        "class Holder "
        "  private _o: Ordinary; "
        "  public constructor() "
        "  end constructor; "
        "end Holder; "
        "class Program "
        "  public static method Main(): int "
        "    var o := default(Ordinary); "
        "    using var h := new Holder(); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// A constructor-only-typed field with an INITIALIZER stays accepted at
// every site — the spec's positive control at s9_9_0007. The fix must
// narrow to the uninitialized case, never reject an explicit constructor
// call. The returned port value must round-trip through the field — so
// the test passes a value < 255 (WEXITSTATUS masks the upper bits, so
// 8080 reads as 144 — CLAUDE.md vacuous-oracle catalogue).
BEZEL_TEST(record_is_constructor_only_4699,
           constructor_only_field_with_initializer_accepted_positive_control) {
    auto r = runAot(
        std::string("module M; ") + kPort +
        "class Listener "
        "  private _port: Port := new Port(80); "
        "  public constructor() "
        "  end constructor; "
        "  public method Number(): int => _port.Value; "
        "end Listener; "
        "class Program "
        "  public static method Main(): int "
        "    using var l := new Listener(); "
        "    return l.Number(); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 80);
}

// A constructor-only record remains auto-hashable and equality-comparable
// (the §9.2 membership rule keeps `=`/`<>` alive for any body record with
// an auto-property and a non-disqualifying member; a constructor is a
// non-disqualifying member, so the package still synthesizes). The
// runtime values are True (same instance) and False (different Value).
// This is the spec's positive control at s9_9_0005; the fix must not
// regress it.
BEZEL_TEST(record_is_constructor_only_4699,
           constructor_only_record_remains_auto_hashable_positive_control) {
    auto r = runAot(
        std::string("module M; ") + kPort +
        "class Program "
        "  public static method Main(): int "
        "    var a := new Port(8080); "
        "    var b := new Port(8080); "
        "    var c := new Port(443); "
        "    if a = b then Console.WriteLine(\"True\"); "
        "    else Console.WriteLine(\"False\"); end if; "
        "    if a = c then Console.WriteLine(\"True\"); "
        "    else Console.WriteLine(\"False\"); end if; "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    // stdout: True\nFalse (each on its own line).
    BEZEL_EXPECT_TRUE(r.stdoutText.find("True") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("False") != std::string::npos);
}