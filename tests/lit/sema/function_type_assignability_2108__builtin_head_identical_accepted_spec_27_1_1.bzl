// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// fix #2108 / §27.1.1 (v0.226): companion ACCEPT control for the reject case
// (function_type_assignability_2108__builtin_head_equality_rejected_spec_27_1_1).
// The fix distinguishes distinct null-declaration builtin heads by headName,
// but must NOT break legitimate equality of IDENTICAL builtin heads — a
// `(Set<int>) -> int` value is assignable to a `(Set<int>) -> int` target
// (parameter slot, same head + same generic arg). Same construction as the
// reject test with the target head unchanged: must compile clean.

module M; class C public static method TakeSet(s: Set<int>): int return 0; end TakeSet; public static method Main(): int var f: (Set<int>) -> int := TakeSet; var g: (Set<int>) -> int := f; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
