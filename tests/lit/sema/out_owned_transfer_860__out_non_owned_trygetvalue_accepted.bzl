// Ported from tests/sema/out_owned_transfer_860_test.cpp
//   (BEZEL_TEST out_owned_transfer_860, out_non_owned_trygetvalue_accepted).
//
// §28.3 / §28.11 / #860 (OUT-NON-OWNED GUARDRAIL): an ordinary `out` (non-
// owned) parameter `Dictionary.TryGetValue(key, out value)` — `out`, not
// `out owned` — carries no discharge obligation. Dropping `value` must be
// ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int do const d: Dictionary<string, int> := ["a": 5]; var v: int? := null; var ok := d.TryGetValue("a", out v); end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
