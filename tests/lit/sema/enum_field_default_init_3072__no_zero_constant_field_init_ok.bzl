// fix #3072 / §7.9 (v0.247) / §17.11: a class field of a zero-less enum is
// §7.9-strict (§7.9 carve-out: enums with no declared zero-valued constant).
// An IN-CLASS initializer discharges the obligation — the field is assigned
// at construction step 2 (§22.8), the same as the non-nullable-reference-field
// case pinned by owned_field_da_test.owned_field_with_initializer_ok_
// without_assignment.
//
// Paired with enum_field_default_init_3072__no_zero_constant_field_diagnoses:
// same `_l: Level` on a zero-less `Level`, the ONLY difference is the
// presence of an in-class initializer. Expected: clean compile.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
enum Level Low := 1, Mid := 2; end Level;
class C
  private _l: Level := Level.Low;
  public method Read(): int => int(_l);
  public static method Main(): int
    using var c := new C();
    return c.Read();
  end Main;
end C;

// NODIAG-NOT: non-nullable field `_l`
// NODIAG-NOT: §7.9
