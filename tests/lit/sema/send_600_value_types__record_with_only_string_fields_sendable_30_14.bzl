// Ported from tests/sema/send_600_sendability_value_types_test.cpp
//   (BEZEL_TEST send_600_value_types, record_with_only_string_fields_sendable_30_14).
//
// §30.14: a record with only string fields is NOT borrow-bearing (string is
// runtime-managed, §9.7) → sendable. Channel<NamedRecord> is accepted.
//
// fix #4001 (§7.9, v0.294): `Name`/`Tag` are non-nullable `string` body fields
// with no field initializer and (pre-fix) no constructor — exactly the gap
// #4001 closes (a class with the identical shape was ALREADY rejected before
// this fix; only the record twin was not). Field initializers here discharge
// the §7.9 obligation without changing either field's TYPE, so the §30.14
// sendability shape under test — a record with only string fields — is
// unaffected.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record NamedRecord public Name: string := ""; public Tag: string := ""; end NamedRecord; class C public static method Test() using var ch := new Channel<NamedRecord>(); end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
