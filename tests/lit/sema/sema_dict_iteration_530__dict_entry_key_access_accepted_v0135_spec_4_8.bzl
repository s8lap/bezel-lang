// Ported from tests/sema/dict_iteration_diag_530_test.cpp
//   (BEZEL_TEST sema_dict_iteration_530, dict_entry_key_access_accepted_v0135_spec_4_8).
//
// §4.8 v0.135 (#548): `for var e in d do ... e.Key ...` must be accepted.
// entry.Key is typed as K (string for Dictionary<string,int>).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class V end V; class C public method M( d: Dictionary<string, int>, od: OwningDictionary<string, V>, arr: Array<int>, s: Set<int>, n: int) for var e in d do var k: string := e.Key; n := n + 1; end; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
