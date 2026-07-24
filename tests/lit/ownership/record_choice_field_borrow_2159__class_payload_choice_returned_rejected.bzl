// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §35.4 (v0.226, issue #2159): a record with a borrow-bearing CHOICE
// field is a borrow-bearing record by transitivity — §9.7's borrow-carrying
// field list includes "a borrow-bearing choice (§35.4) (transitivity)", and
// §35.4.2 makes `RefC` borrow-bearing (an unmarked class payload `r: Res`).
// Before the fix, both record field predicates fell through to `return false;
// // enum` for a Choice-kind field (classifyChoiceType was never consulted), so
// `HolderC` was NOT classified borrow-bearing and this §9.7 return escape —
// the issue's exact repro — compiled clean while `local` auto-drops, leaving the
// returned record's carried borrow dangling.

module M; class Res public method Val(): int return 5; end Val; end Res; choice RefC Some(r: Res), None; end RefC; record HolderC(K: RefC); class C public static method Leak(): HolderC var local := new Res(); return new HolderC(RefC.Some(local)); end Leak; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned borrow-bearing record carries a borrow of `local` whose source is disposed at scope end
// CHECK-SAME: (§9.7 record borrow-lifetime rule
