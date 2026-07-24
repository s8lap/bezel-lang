// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §35.11 / §35.12 (v0.226, issue #2211): per-instantiation substitution does
// not weaken the "every payload is sendable" requirement. `Opt<Object>` is
// value-like, but `Object` is not sendable under §30.14, so the constraint is
// rejected. The component-path diagnostic must name the instantiated payload
// type `Object`, not the declaration template's bare `T`.

module M;
choice Opt<T> Some(v: T), None; end Opt;
static class Box
  public static method S<T>(): int where T: sendable => 1;
end Box;
class C
  public static method Main(): int
    return Box.S<Opt<Object>>();
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Opt` does not satisfy the constraint `where T: sendable` (§14.2): payload `v` of type `Object` is not sendable (§35.12)
