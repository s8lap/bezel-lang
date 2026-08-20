// Spec-derived tests for fix #3072 — §7.9 (v0.247) / §17.11 default-
// initialized enum storage.
//
// §7.9 (v0.247) carves a field's "must have an explicit initializer" set
// out of value types whose default is not well-defined: in addition to the
// non-nullable reference fields the prior rounds already enforced, an enum
// with no declared zero-valued constant — `default(E)` is a compile error
// for it per §17.11 — joins the set. Without the check, the slot is zero-
// filled and the program observes a `Level` value of 0, equal to NO declared
// constant — exactly the state §5.4's invariant forbids and the state §19.6's
// exhaustiveness guarantee and §21.7's totality grant rest on.
//
// The lit family (tests/lit/sema/enum_field_default_init_3072__*.bzl) pins
// the diagnostic. THIS file pins the runtime face of the fix: each accepted
// shape (zero-valued enum, field initializer, explicit ctor assignment) must
// still read back the value through the program's normal read path, so a
// false-reject cost would show up as a flaky compile-or-rc mismatch here.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// §7.9 / §17.11: a zero-less enum. self-contained — kept private to this
// file so the test cases read declaratively.
const char* kZeroLessEnum =
    "module M; "
    "enum Level Low := 1, Mid := 2; end Level; ";

// §7.9 OVER-REJECTION GUARD: an enum that DECLARES a zero-valued constant
// has a well-defined default (§17.11's enum row) and stays accepted.
const char* kZeroEnum =
    "module M; "
    "enum Level Off := 0, Low := 1, Mid := 2; end Level; ";

}  // namespace

// =====================================================================
// fix #3072 / §7.9 / §17.11 — REJECT arm
// =====================================================================

// A zero-less enum field without an initializer and without a constructor
// that assigns it is the §7.9 carve-out. The runAot leg compiles the program
// and runs it; before #3072 the slot was zero-filled and the read returned 0
// (the undeclared value), and the program ran with exit 0. The fix turns
// the same source into a compile error.
BEZEL_TEST(sema_enum_field_default_init_3072,
           no_zero_constant_field_without_init_compile_error_spec_7_9) {
    auto r = runAot(std::string(kZeroLessEnum) +
        "class C "
        "  private _l: Level; "
        "  public method Read(): int => int(_l); "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return c.Read(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("non-nullable field `_l`") != std::string::npos &&
            d.message.find("§7.9") != std::string::npos) {
            found = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(found);
}

// =====================================================================
// fix #3072 / §7.9 / §17.11 — ACCEPT arms (over-rejection guards)
// =====================================================================

// A zero-valued enum field has a well-defined default and the program must
// compile AND run, reading the value 0 (the `Off` constant) — value-bearing
// readback so a false reject would show as compile failure here.
BEZEL_TEST(sema_enum_field_default_init_3072,
           zero_constant_field_default_to_zero_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(std::string(kZeroEnum) +
        "class C "
        "  private _l: Level; "
        "  public method Read(): int => int(_l); "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return c.Read(); "
        "  end Main; "
        "end C;"),
        0);
}

// An in-class initializer on a zero-less enum field discharges the §7.9
// obligation — the field is assigned at construction step 2 (§22.8). The
// program reads back the assigned constant's underlying value.
BEZEL_TEST(sema_enum_field_default_init_3072,
           no_zero_constant_field_with_initializer_ok_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(std::string(kZeroLessEnum) +
        "class C "
        "  private _l: Level := Level.Low; "
        "  public method Read(): int => int(_l); "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return c.Read(); "
        "  end Main; "
        "end C;"),
        1);
}

// A constructor that assigns the field on its normal-exit path discharges
// the §7.9 obligation (§22.7 field-DA). The ownership pass's
// no-constructor check (Ownership::isNonNullableReferenceField) does not fire
// because a constructor is present.
BEZEL_TEST(sema_enum_field_default_init_3072,
           no_zero_constant_field_assigned_in_ctor_ok_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(std::string(kZeroLessEnum) +
        "class C "
        "  private _l: Level; "
        "  public constructor() this._l := Level.Mid; end constructor; "
        "  public method Read(): int => int(_l); "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return c.Read(); "
        "  end Main; "
        "end C;"),
        2);
}

// A constructor is present but does NOT assign the zero-less enum field
// on its normal-exit path — §22.7 / DefiniteAssignment's
// checkCtorFieldCompletion must fire. The Ownership.cpp no-constructor check
// does NOT fire (a constructor is present); this exercises the
// DefiniteAssignment.cpp path that the no-constructor diagnosis above does
// not reach. Without #3072's enum extension, this compiled silently and the
// read returned 0 — the undeclared value.
BEZEL_TEST(sema_enum_field_default_init_3072,
           no_zero_constant_field_ctor_unassigned_diagnoses_spec_7_9) {
    auto r = runAot(std::string(kZeroLessEnum) +
        "class C "
        "  private _l: Level; "
        "  public constructor() end constructor; "
        "  public method Read(): int => int(_l); "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return c.Read(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("constructor does not initialize non-nullable field `_l`") != std::string::npos ||
            (d.message.find("non-nullable field `_l`") != std::string::npos &&
             d.message.find("§7.9") != std::string::npos)) {
            found = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(found);
}

// A zero-less enum field with nullable type is exempt from the §7.9
// carve-out (matches the non-nullable-reference-field pinned behavior in
// owned_field_da_test.nullable_reference_field_not_required). Even an empty
// enum default for `Level?` is `null` — a well-defined default.
BEZEL_TEST(sema_enum_field_default_init_3072,
           nullable_zero_less_enum_field_accepted_spec_7_9) {
    BEZEL_EXPECT_EQ(runProg(std::string(kZeroLessEnum) +
        "class C "
        "  private _l: Level?; "
        "  public method Read(): int "
        "    if _l = null then return 1; else return 0; end; "
        "  end Read; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return c.Read(); "
        "  end Main; "
        "end C;"),
        1);
}
