// fix #3910 (§17.11 / §3.6 rule 2 / §17.7 / §26.4, v0.315): same as
// default_enum_zero_3910__extension_level_shorthand_new_rejected.bzl
// but uses the FIELD-INITIALIZER shorthand position (the second of the two
// §17.7 shorthand sites instrumented by this fix) instead of the local-
// declaration position.
//
// `Level` declares 1, 2, 3 (no zero-valued constant).
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;

enum Level Low := 1, Mid := 2, High := 3; end Level;

class Box<K>
  public V: K?;
end Box;

extension Box<T> where T: value
  public method IsZero(): bool
    var t := default(T);
    return t = t;
  end IsZero;
end;

class C
  public owned _b: Box<Level> := new();
  public static method Main(): int
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Level` does not satisfy the constraint `where T: value`
// CHECK-SAME: takes `default(T)`
