// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §14.2, v0.307 / #4689 (companion): the SAME gap for a CHOICE bound
// — §14.2's table has no "named choice bound" row either.

module M;

public choice Opt Some(value: int), None; end Opt;

public class Box<T> where T: Opt
  public V: T?;
end Box;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` is constrained to `Opt`, a choice
// CHECK-SAME: (§14.2)
