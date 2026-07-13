// Ported from tests/sema/static_field_domains_1363_test.cpp
//   (BEZEL_TEST sema_static_domains_1363, rule2_mutable_static_task_generic_cache_admitted).
//
// (21) §22.12 rule 2 form (ii): a mutable static `Task<T>?` lazy cache is
// admitted (the §22.13 pattern (b) null-initialized once-set cache).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cfg public X: int := 0; end Cfg; class App private static _cfg: Task<Cfg>? := null; public static method Main(): int => 0; end App;

// NODIAG-NOT: error
// NODIAG-NOT: §
