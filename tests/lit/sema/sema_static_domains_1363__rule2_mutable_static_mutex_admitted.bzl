// Ported from tests/sema/static_field_domains_1363_test.cpp
//   (BEZEL_TEST sema_static_domains_1363, rule2_mutable_static_mutex_admitted).
//
// (4) §22.12 rule 2 positive: Mutex is in the synchronized set — admitted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class App public static _lk: Mutex := new Mutex(); public static method Main(): int => 0; end App;

// NODIAG-NOT: error
// NODIAG-NOT: §
