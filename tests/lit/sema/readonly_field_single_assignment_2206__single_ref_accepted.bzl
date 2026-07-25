// §22.8 (v0.226) single-assignment of readonly fields (#2206, route A) — ACCEPT.
//
// A SINGLE `ref` argument whose operand is a readonly field is admitted as the
// single assignment (the callee's write through the alias is the one assignment
// §22.8 permits, same as a single `out`). The over-reject failure mode would
// flag ANY `ref` of a readonly field; this pins that a first, non-loop `ref` is
// accepted. NB: a `ref` AFTER a `:=` of the same field is a second write and is
// rejected (see readonly_field_single_assignment_2206__ref_second_write_rejected).
// No diagnostic.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public readonly _f: string?; public constructor() Probe(ref _f); end constructor; destructor end destructor; public method Probe(ref x: string?) x := "a"; end Probe; public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §22.8
