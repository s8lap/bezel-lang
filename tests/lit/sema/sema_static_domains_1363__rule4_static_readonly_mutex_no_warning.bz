// Ported from tests/sema/static_field_domains_1363_test.cpp
//   (BEZEL_TEST sema_static_domains_1363, rule4_static_readonly_mutex_no_warning).
//
// (15) §22.12 rule 4 positive: static readonly Mutex — warning suppressed
// (Mutex is in the synchronized set).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class App public static readonly _lk: Mutex := new Mutex(); public static method Main(): int => 0; end App;

// NODIAG-NOT: error
// NODIAG-NOT: §
