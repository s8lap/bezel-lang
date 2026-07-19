// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, record_conformance_accepted).
//
// RECORD conformance: a record supplies the statics — accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
// v0.201 (#2048): the interface's producer static is spelled `owned T` per the
// declaration-site static-return rule. At the RECORD (value) conformer the
// marker is erased by the substitution (§28.1, #2063) — the witness stays
// `Zero(): Pt` and matches the erased interface signature.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; public interface IZ<T> where T: IZ<T> static method Zero(): owned T; end IZ; public record Pt(x: int): IZ<Pt> public static method Zero(): Pt => new Pt(0); end Pt;

// NODIAG-NOT: error
// NODIAG-NOT: §
