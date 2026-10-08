// fix #5610 (§35.2, v0.315): the verdict-flipping shape for choices.
// Symmetric to the enum case: overriding BOTH Equals(int) and GetHashCode
// bypasses the §8.3 co-declaration check; pre-fix this compiles clean,
// post-fix it is rejected with the §35.2 diagnostic.
//
// REJECT: override Equals(int) + GetHashCode on choice — §35.2 violation.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; choice Color Red(payload: int), Green, Blue(msg: string); public override method Equals(x: int): bool => true; public override method GetHashCode(): int => 1; end Color; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: choice method `Equals` may not be virtual, abstract, or override — choices dispatch statically (§35.2)
