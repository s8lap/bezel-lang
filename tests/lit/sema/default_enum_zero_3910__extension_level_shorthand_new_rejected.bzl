// fix #3910 (§17.11 / §3.6 rule 2 / §17.7 / §26.4, v0.315): the §17.7
// `new()` shorthand was not instrumented to record instantiation sites, so
// an extension-level `default(T)` obligation was not discharged at a shorthand
// construction site. This is the EXTENSION-LEVEL sibling of the class-level
// shorthand fix (#3910 field-initializer and local-declaration sites).
//
// The explicit form `new Box<Level>()` is already tested by
// default_enum_zero_3719__extension_level_generic_value_instantiation_rejected.bzl.
// This test uses the shorthand form `new()` at a local-declaration position.
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
  public static method Main(): int
    var b: Box<Level> := new();
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Level` does not satisfy the constraint `where T: value`
// CHECK-SAME: takes `default(T)`
