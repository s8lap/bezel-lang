// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, char_steppable_not_inumber —
//   first of two independent parseSemaAll() assertions in that BEZEL_TEST
//   body, split into its own .bzl per the extractor's manual-split
//   convention).
//
// §2.19: `char` conforms to ISteppable but NOT to INumber. This half checks
// the ISteppable-constrained call is accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class F   public static method Nxt<T>(v: T): T where T: ISteppable<T>     return T.Step(v, 1L); end Nxt; end F; class C public static method Main(): int   var c := F.Nxt('a'); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
