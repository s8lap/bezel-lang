// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §14.2, v0.307 / #4689 (companion): the SAME gap the record bound
// has — §14.2's table has no "named ENUM bound" row either, only class
// and interface. Proves the fix is a genuine KIND check (rejects every
// non-class/interface named type), not a record-only special case.

module M;

public enum Color Red, Green, Blue; end Color;

public class Box<T> where T: Color
  public V: T?;
end Box;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` is constrained to `Color`, a enum
// CHECK-SAME: (§14.2)
