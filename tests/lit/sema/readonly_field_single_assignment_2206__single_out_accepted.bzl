// §22.8 (v0.226) single-assignment of readonly fields (#2206, route A) — ACCEPT.
//
// A SINGLE `out` argument whose operand is a readonly field is the legitimate
// single assignment (placement is allowed by checkRefOutFieldOperand while
// `inConstructor_`; the callee's write is the one assignment §22.8 permits).
// The over-reject failure mode would wrongly treat the `out` as a second
// write; the once-check admits a first, non-loop write and records it. No
// diagnostic.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public readonly _f: string?; public constructor() SetIt(out _f); end constructor; destructor end destructor; public method SetIt(out x: string?) x := "a"; end SetIt; public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §22.8
