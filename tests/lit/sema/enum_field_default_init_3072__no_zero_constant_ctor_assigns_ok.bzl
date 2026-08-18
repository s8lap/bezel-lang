// fix #3072 / §7.9 (v0.247) / §17.11: a class field of a zero-less enum is
// §7.9-strict. A constructor that assigns the field on its normal-exit path
// discharges the obligation — the same path the non-nullable-reference-field
// case satisfies (owned_field_da_test.owned_field_without_init_assigned_in_
// ctor_ok). The §22.7 field-DA join is the all-paths join owned by
// DefiniteAssignment::isSevenNineStrictField; the ownership pass's
// no-constructor check (Ownership::isNonNullableReferenceField) does not fire
// because a constructor is present.
//
// Paired with enum_field_default_init_3072__no_zero_constant_field_diagnoses:
// same `_l: Level` on a zero-less `Level`, the ONLY difference is an explicit
// constructor that assigns `_l`.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
enum Level Low := 1, Mid := 2; end Level;
class C
  private _l: Level;
  public constructor() this._l := Level.Low; end constructor;
  public method Read(): int => int(_l);
  public static method Main(): int
    using var c := new C();
    return c.Read();
  end Main;
end C;

// NODIAG-NOT: non-nullable field `_l`
// NODIAG-NOT: §7.9
