// fix #3044 (§17.11, v0.242) OVER-REJECTION GUARD: `default(E)` stays accepted
// for an enum that DOES declare a zero-valued constant — §17.11's enum row
// reads "Enums: the value with underlying integer 0 — necessarily a declared
// constant, per the compile error below". `Level` declares `Off := 0`, so
// `default(Level)` is `Off`.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Level Off := 0, Low := 1, High := 2; end Level; class C public static method Main(): int var l: Level := default(Level); return int(l); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
