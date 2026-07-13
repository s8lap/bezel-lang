// Ported from tests/sema/bool_condition_test.cpp
//   (BEZEL_TEST sema_cond, coalesced_nullable_bool_condition_ok_r39).
//
// Control: a coalesced `bool?` (`b ?? false`) is a plain `bool` and is fine.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method M(flag: bool, n: int) var b: bool? := null; if (b ?? false) then n := 1; end; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
