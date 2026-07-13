// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, owned_return_mismatch_rejected).
//
// The `owned` return modifier participates in the exact-match rule (§11.2):
// an implementation dropping the interface's `owned` is a mismatch.

module M; public interface IFac<T> where T: IFac<T> static method Make(): owned T; end IFac; public class W: IFac<W> public static method Make(): W => new W(); end W;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: class `W` implements static interface member `Make` of `IFac` with a different signature — the implementing static must match the interface member exactly: same parameter count, parameter types and modifiers, and return type including `owned`
// CHECK-SAME: (§11.2 / §11.8)
