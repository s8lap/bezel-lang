// Ported from tests/sema/getoradd_argcheck_542_test.cpp
//   (BEZEL_TEST sema_getoradd_argcheck_542, array_select_with_valid_selector_accepted_spec_5).
//
// §5 / #542: `arr.Select((x) => x * 2)` — a LINQ Select with a valid
// selector lambda. Must not be broken by the factory-arg check.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=WARN --match-full-lines %s

module M; class Val end Val; class C public method M( d: Dictionary<string, int>, od: OwningDictionary<string, Val>, arr: Array<int>, n: int) var e := arr.Select(lambda(x) => x * 2); end M; end C;

// WARN: warning: owned value in `e` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop)
