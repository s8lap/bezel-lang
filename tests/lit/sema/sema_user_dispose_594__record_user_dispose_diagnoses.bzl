// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/user_dispose_method_594_test.cpp
//   (BEZEL_TEST sema_user_dispose_594, record_user_dispose_diagnoses).
//
// §28.2: Dispose() on a record is also rejected (records cannot have a
// destructor per §9.7, but the user might still write a Dispose() method
// hoping it runs — it won't).

module M; record Point(x: int, y: int) public method Dispose() _ := 0; end Dispose; end Point;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `Point` may not declare a `Dispose()` method directly
// CHECK-SAME: (§28.2)
