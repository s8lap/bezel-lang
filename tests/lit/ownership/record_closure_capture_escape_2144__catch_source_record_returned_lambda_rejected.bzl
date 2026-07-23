// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §31.1 (v0.220, issue #2144): the CATCH-SOURCE kind — the captured
// borrow-bearing record wraps the caught exception (`ErrRec(ex, 4)`), whose
// lifetime is the catch arm body (the runtime disposes it after the arm
// completes, §31.1). A closure capturing the record and escaping the arm via
// `return` would outlive the exception exactly as one capturing `ex`
// directly (the #392 rule), reached here through the record's carried-source
// resolution.

module M; class MyErr: Exception public constructor() base("boom"); end constructor; end MyErr; record ErrRec(E: MyErr?, N: int); class C public static method MakeLeak(): () -> int try throw new MyErr(); catch var ex: MyErr => do var h := new ErrRec(ex, 4); return lambda(): int => h.N; end end try; return lambda(): int => 0; end MakeLeak; public static method Main(): int var f := MakeLeak(); return f(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned closure captures `h`, which carries a borrow of the caught exception `ex`
// CHECK-SAME: (§9.7 / §31.1
