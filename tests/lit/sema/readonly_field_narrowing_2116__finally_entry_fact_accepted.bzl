// §7.4 (v0.214) / §31.2 / #2116 (pre-gate review): an AMBIENT readonly-field
// fact that holds at TRY ENTRY is valid on every path into the finally —
// normal completion, early returns, unwind, catch exits — because an ambient
// fact is never invalidated. The finally use and the after-try use are both
// ACCEPTED. No diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; public method Noise() end Noise; public method Len(): int if _name <> null then try Noise(); catch Exception => do var y := 0; end; finally var x := _name.Length; end; return _name.Length; end; return 0; end Len; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
