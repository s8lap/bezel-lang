// Ported from tests/sema/with_expression_target_validation_875_test.cpp
//   (BEZEL_TEST with_expression_target_validation_875, auto_property_target_accepted).
//
// §9.5, #875-878 positive: an auto-property with backing storage is a valid
// `with` target.
//
// fix #4001 (§7.9, v0.294): `Name` is a non-nullable `string` body field with
// no field initializer and (pre-#4001) no constructor — exactly the gap #4001
// closes (a class with the identical shape was ALREADY rejected before that
// fix; only the record twin was not, letting `new Person()` leave `Name`
// unset). The field initializer discharges §7.9 without touching `Age`, the
// actual `with` target this test exercises.
//
// fix #4059 (§9.1 / §9.2, v0.295): `Person` declares a parameterless
// `constructor()` so that `new Person()` is admitted — without it, fix
// #4059 rejects the call as a body record with no implicit default ctor
// (the rejection shape is exercised by the sibling lit test
// `body_only_record_zero_arg_new_4059` and by the gtest rows under
// `tests/sema/body_only_record_zero_arg_new_4059_test.cpp`). The
// parameterless ctor mirrors the typical "user wants a default-value
// construction path" pattern; the field initializer `Name := ""` is the
// real §7.9 discharge for the test's purpose.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Person public Name: string := ""; public Age: int; public constructor() end constructor; end Person; class C public static method Main(): int var p := new Person(); var q := p with(Age := 31); return q.Age; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
