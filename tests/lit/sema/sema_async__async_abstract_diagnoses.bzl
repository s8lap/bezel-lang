// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/async_abstract_test.cpp
//   (BEZEL_TEST sema_async, async_abstract_diagnoses).

module M; abstract class C public async abstract method F(): Task; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: method `F` may not be both `async` and `abstract`; declare a `Task` / `Task<T>` return type without `async`
// CHECK-SAME: (§12.6)
