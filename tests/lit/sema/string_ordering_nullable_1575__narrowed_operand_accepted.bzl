// Ported from tests/sema/string_ordering_nullable_1575_test.cpp
//   (BEZEL_TEST string_ordering_nullable_1575, narrowed_operand_accepted).
//
// §7.4 (#1575): a `string?` operand narrowed first (`<> null` guard) is
// accepted — the fix must not regress the narrow-then-use idiom that §4.6
// explicitly points callers toward.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var a: string? := "a"; var b: string? := "b"; if a <> null and b <> null then if a < b then return 1; end; end; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
