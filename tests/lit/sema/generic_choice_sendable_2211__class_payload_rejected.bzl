// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §35.11 / §35.12 (v0.226, issue #2211): an unmarked class substitution makes
// `Opt<Res>` borrow-bearing (§35.4.2), and borrow-bearing choices are never
// sendable. Instantiation-aware payload substitution must not weaken this
// genuinely non-sendable axis.

module M;
choice Opt<T> Some(v: T), None; end Opt;
class Res public method V(): int => 1; end Res;
static class Box
  public static method S<T>(): int where T: sendable => 1;
end Box;
class C
  public static method Main(): int
    return Box.S<Opt<Res>>();
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Opt` does not satisfy the constraint `where T: sendable` (§14.2)
