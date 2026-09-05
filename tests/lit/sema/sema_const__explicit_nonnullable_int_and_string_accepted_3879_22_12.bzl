// fix #3879 (§22.12, v0.294) OVER-REJECTION GUARD: the `nullable` clause
// this fix adds to the shared predicate (`isAdmissibleClassConstType`,
// ConstFold.hpp) must not reject the non-nullable forms of the same two
// rows — a plain `int` const (the primitive-or-enum row's "non-nullable
// primitive: ok") and a plain `string` const (the `string`/`string?` row's
// "`string`: ok") must keep compiling exactly as before.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public const A: int := 1; public const S: string := "hi"; public static method Main(): int => A; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
