// Ported from tests/sema/try_convention_1336_test.cpp
//   (BEZEL_TEST try_convention_1336_sema, discard_recipient_accepted).
//
// (e) `out _` discard — ACCEPT (no recipient to type-check).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int do const d: Dictionary<string, int> := ["a": 1]; if d.TryGetValue("a", out _) then return 1; end; end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
