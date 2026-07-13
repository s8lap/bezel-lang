// Ported from tests/sema/static_field_domains_1363_test.cpp
//   (BEZEL_TEST sema_static_domains_1363, rule2_mutable_static_task_bare_cache_admitted).
//
// (22) §22.12 rule 2 form (ii): the non-generic `Task?` cache is likewise
// admitted (the admission is keyed to the declared type alone).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class App private static _work: Task? := null; public static method Main(): int => 0; end App;

// NODIAG-NOT: error
// NODIAG-NOT: §
