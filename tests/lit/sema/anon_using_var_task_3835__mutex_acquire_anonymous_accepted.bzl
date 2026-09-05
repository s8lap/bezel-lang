// Issue #3835 / spec §17.13 (v0.288, axiom A6): false-reject control. The new
// rejection is keyed on the initializer being Task-shaped (`isTaskShapedRef`)
// — a non-Task owned producer bound anonymously, the sanctioned
// `using var _ := mutex.Acquire();` idiom stdlib §6.2 names directly, must
// stay legal.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; import System.Threading; class C public static method Test() using var m := new Mutex(); using var _ := m.Acquire(); end Test; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
