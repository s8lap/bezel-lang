// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, record_missing_static_rejected).
//
// RECORD conformance: a record missing the static — rejected.

module M; public interface IZ<T> where T: IZ<T> static method Zero(): T; end IZ; public record Pt(x: int): IZ<Pt> end Pt;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `Pt` does not implement static interface member `Zero` of `IZ` — a type declaring conformance to an interface with static members must supply each of them as a `public static` member
// CHECK-SAME: (§11.8)
