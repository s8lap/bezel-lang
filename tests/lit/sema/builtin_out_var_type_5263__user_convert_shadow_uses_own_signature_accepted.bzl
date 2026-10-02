// Issue #5263 / spec §22.4 (v0.313): a USER type named `Convert` shadows the
// builtin; its own `out r: int` declaration types the inline recipient, so
// `var n: int := d` is ACCEPTED (the builtin `double` typing must not leak in
// by name).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; public class Convert public static method TryToDouble(s: string, out r: int): bool r := 41; return true; end TryToDouble; end Convert; class C public static method Main(): int var ok := Convert.TryToDouble("x", out var d); var n: int := d; return n; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
