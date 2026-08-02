// §22.3 / §14.5 (spec v0.228) / #2266 over-rejection control: a generic method
// whose ref parameters are MIXED — one generic (`ref g: T`, §14.5's domain) and
// one concrete (`ref c: int`, §22.3's). §14.5 must keep owning the generic slot
// (the parameter type arrives as the unsubstituted `T`, which no concrete
// argument type structurally equals, so applying the §22.3 exact comparison to
// it would false-reject), while §22.3 checks the concrete slot. Neither may
// reject, and neither may double-report.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class U public static method Both<T>(ref g: T, ref c: int) c := c + 1; end Both; end U; class C public static method Main(): int var g: int := 1; var c: int := 2; U.Both<int>(ref g, ref c); return g + c; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
