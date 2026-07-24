// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §35.4.1 (v0.226, issue #2210): the body-record form. A body
// record's auto-property (`property K: T;`) is also a storage-bearing
// field, so the same §35.4 rejection fires — the diagnostic is named
// separately so a reader can identify the syntactic shape (the positional
// and body forms must both reject; the wording is shaped by which form
// carries the type).

module M; class Res public method V(): int return 5; end V; end Res; choice Own Some(owned r: Res), None; end Own; record HolderO public property K: Own; end HolderO; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `HolderO` may not have an owning-classified choice auto-property `K`
// CHECK-SAME: (§35.4, §9.7)
