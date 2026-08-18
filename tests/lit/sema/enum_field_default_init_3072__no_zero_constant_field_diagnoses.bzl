// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3072 / §7.9 (v0.247) / §17.11: a class field of an enum type that
// declares no zero-valued constant has no well-defined default. §7.9 puts it
// in the same per-field carve-out as a non-nullable reference field — the
// field requires an explicit initializer (or a constructor that assigns it on
// every normal-exit path, §22.7). Without the check, the storage is zero-
// filled and the program observes a `Level` value of 0, equal to NO declared
// constant — exactly the state §5.4's invariant forbids.
//
// `Level` declares 1, 2 — no constant of underlying value 0. The class
// declares an instance field `_l: Level` with no initializer AND no
// constructor that assigns it.

module M;
enum Level Low := 1, Mid := 2; end Level;
class C
  private _l: Level;
  public method Read(): int => int(_l);
  public static method Main(): int
    using var c := new C();
    return c.Read();
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: non-nullable field `_l` needs an initializer or a constructor that assigns it
// CHECK-SAME: (§7.9)
