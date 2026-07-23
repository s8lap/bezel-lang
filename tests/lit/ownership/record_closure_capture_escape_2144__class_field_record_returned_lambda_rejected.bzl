// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 (v0.220, issue #2144): the same closure-capture escape with a CLASS-
// field borrow-bearing record — the issue's "not span-specific" control. `h`
// carries a borrow of `local` (a plain `Res?` reference field, §9.7's first
// borrow-carrying-field kind); the returned lambda captures `h` and escapes,
// outliving `local`'s §28.3 auto-drop. Rejected even though the lambda body
// reads only the record's VALUE field `h.N` — the §9.7 closure bullet is a
// rule on the captured record value, not on which fields the body touches
// (the capture holds the whole record, dangling borrow included).

module M; class Res public method Val(): int return 5; end Val; end Res; record RefRec(W: Res?, N: int); class C public static method MakeLeak(): () -> int var local := new Res(); var h := new RefRec(local, 3); return lambda(): int => h.N; end MakeLeak; public static method Main(): int var f := MakeLeak(); return f(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: returned closure captures `h`, a borrow-bearing record carrying a borrow of `local` whose source is disposed at scope end — a borrow-bearing record may not be captured by a closure that escapes its source's scope
// CHECK-SAME: (§9.7 closure-capture rule
