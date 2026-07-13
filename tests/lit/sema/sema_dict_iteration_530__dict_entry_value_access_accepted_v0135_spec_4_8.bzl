// Ported from tests/sema/dict_iteration_diag_530_test.cpp
//   (BEZEL_TEST sema_dict_iteration_530, dict_entry_value_access_accepted_v0135_spec_4_8).
//
// §4.8 v0.135 (#548): `for var e in d do ... e.Value ...` must be accepted.
// entry.Value is typed as V (int for Dictionary<string,int>), so the loop body
// can read it without compile error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class V end V; class C public method M( d: Dictionary<string, int>, od: OwningDictionary<string, V>, arr: Array<int>, s: Set<int>, n: int) for var e in d do n := n + e.Value; end; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
