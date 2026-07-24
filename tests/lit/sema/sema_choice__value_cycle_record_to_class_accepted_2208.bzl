// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// #2208 (§35.1 + §9, v0.226): a record whose field is a CLASS is a heap
// reference; the class breaks the value-typed cycle. The record-side check
// stays quiet and the choice-side check (recordsAllowed = false) also stays
// quiet (no record on the cycle path either). This is the non-cyclic
// record→class positive control.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// §35.1/§9 self-containment diagnostic.

module M; class Inner end Inner; record Wrap(K: Inner); class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: cannot recursively contain itself by value
// NODIAG-NOT: cannot recursively contain itself by value through its case payloads
// NODIAG-NOT: cannot recursively contain itself through its case payloads
