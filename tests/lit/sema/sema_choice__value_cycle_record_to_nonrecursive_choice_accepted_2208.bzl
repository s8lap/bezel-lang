// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// #2208 (§35.1 + §9, v0.226): a record whose field is a non-recursive
// choice is fine. The choice's payload fields are all primitives — no
// value-typed component points back to the record. This is the non-cyclic
// record→choice control, the kind of layout that #2208's repro graph is
// trying to simulate.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// §35.1/§9 self-containment diagnostic.

module M; choice Color Red, Green(value: int), Blue; end Color; record Wrap(C: Color); class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: cannot recursively contain itself by value
// NODIAG-NOT: cannot recursively contain itself by value through its case payloads
// NODIAG-NOT: cannot recursively contain itself through its case payloads
