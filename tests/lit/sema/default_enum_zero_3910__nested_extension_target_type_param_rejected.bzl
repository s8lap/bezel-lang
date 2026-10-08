// fix #3910 (§26.4 / §17.11 / §3.6 rule 2, v0.315): the extension half of
// the nested walk fix. `recordDefaultObligationsForConstruction`'s extension-level
// walk used to stop at the top-level generic argument — `extension Box<Array<T>>`
// has `Array<T>` at the first (and only) position, and the `!ga->genericArgs.empty()`
// guard rejected it without recursing, so `T` at the NESTED position was never
// bound and the instantiation site for `new Box<Array<Level>>()` was recorded
// without any entry for `T`. The `default(T)` obligation recorded in the
// extension body therefore had nothing to join against.
//
// `Level` declares 1, 2, 3 (no zero-valued constant); `T` in the extension's
// `where T: value` is bound at the nested position `Array<T>`.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;

enum Level Low := 1, Mid := 2, High := 3; end Level;

class Box<V>
  public V: V?;
end Box;

extension Box<Array<T>> where T: value
  public method IsZero(): bool
    var t := default(T);
    return t = t;
  end IsZero;
end;

class C
  public static method Main(): int
    var b: Box<Array<Level>> := new();
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Level` does not satisfy the constraint `where T: value`
// CHECK-SAME: takes `default(T)`
