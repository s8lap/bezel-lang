// fix #3879 (§22.12, v0.294) OVER-REJECTION GUARD: an inferred class-level
// `const` whose conditional-expr initializer's two arms are BOTH non-null
// (`(if true then 1 else 2)`, inferring to plain `int`, not `int?`) must
// keep compiling — the fix's `nullable` clause must not fire on a
// conditional expression merely because it is a `ConditionalExpr`; only an
// initializer that actually infers a nullable type is rejected (see the
// sibling `inferred_nullable_int_rejected` test, whose `else null` arm is
// what makes the difference).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public const B := (if true then 1 else 2); public static method Main(): int => B; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
