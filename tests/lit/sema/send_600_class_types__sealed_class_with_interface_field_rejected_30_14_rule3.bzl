// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/send_600_sendability_class_types_test.cpp
//   (BEZEL_TEST send_600_class_types, sealed_class_with_interface_field_rejected_30_14_rule3).
//
// §30.14 Rule 3: sealed class with an interface-typed field → not sendable
// ("no interface-typed fields — boxed contents cannot be statically verified").

module M; interface IWorker method Work(): int; end IWorker; sealed class Job public owned Worker: IWorker?; destructor end destructor; end Job; class C public static method Test() using var ch := new Channel<Job>(); end Test; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Job` of `Channel<T>` is not sendable: the element type must satisfy the §30.14 sendable-type rules (not borrow-bearing, not move-only, not function-typed, not domain-local; class types must be transfer-closed/sealed)
// CHECK-SAME: (§30.14/§30.15, #600)
