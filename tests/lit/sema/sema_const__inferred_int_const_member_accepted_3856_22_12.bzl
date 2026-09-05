// fix #3856 (§22.12, v0.294) OVER-REJECTION GUARD: the new inferred-const
// classification added alongside the enum-rejection fix in this file's
// sibling test must not reject an inferred const whose initializer's type
// genuinely IS a primitive value type or `string` — the same clean-compile
// guarantee the explicitly `: int`-typed form already has
// (sema_const__int_const_ok.bzl).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public const K := 5; public const S := "hi"; public static method Main(): int => K; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
