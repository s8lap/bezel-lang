// fix #5610 (§10.5, v0.315): the verdict-flipping shape. Overriding BOTH
// Equals and GetHashCode bypasses the §8.3 co-declaration check, exposing
// the real defect: pre-fix, this compiles clean (exit 0); post-fix, it is
// rejected with the §10.5 diagnostic. The diagnostic-precedence rows above
// cover the single-Equals case where §8.3 fires first; this row covers the
// full-compile shape where §8.3 is satisfied.
//
// REJECT: override Equals(int) + GetHashCode on enum — §10.5 violation.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; enum Color Red, Green, Blue; public override method Equals(x: int): bool => x >= 0; public override method GetHashCode(): int => 1; end Color; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum method `Equals` may not be virtual, abstract, or override — enums dispatch statically (§10.5)
