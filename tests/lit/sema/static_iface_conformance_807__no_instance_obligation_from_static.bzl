// Ported from tests/sema/static_interface_conformance_807_test.cpp
//   (BEZEL_TEST static_iface_conformance_807, no_instance_obligation_from_static).
//
// A static interface member creates NO instance obligation: the conforming
// class needs no instance `Zero` (would otherwise trip §12.5's abstract check).
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; public interface IZ<T> where T: IZ<T> static method Zero(): T; method Scale(k: int): int; end IZ; public class W: IZ<W> public static method Zero(): W => new W(); public method Scale(k: int): int => k; end W;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: static factory method `Zero` returns `W` with no `owned` qualifier and no structural borrow source; the caller treats the result as owned
// CHECK-SAME: (§28.3 sourceless-return rule)
