// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §28.3 arm-meet (v0.184/v0.196; issue #1661): the ownership of an
// expression-position `case`/`when` follows the same arm-meet as an if-
// expression (spec §28.3 producers row; §19.2) — a MIXED owned/borrowed arm set
// is REJECTED. Here the `1` arm is a fresh owned producer and the `_` arm is a
// borrow (`borrowed`). Before the #1661 fix this was silently accepted and the
// fresh producer leaked.
//
// Fails-without / passes-with: MethodOwnership::checkArmMeetMixed reaches the
// case arms through CaseArm::exprBody; without the fix the program compiles
// clean and FileCheck fails to match.

module M;
class Res : IDisposable
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
  destructor end destructor;
end Res;
class W
  public static method Main(): int
    var n := 1;
    using var owner := new Res(9);
    var borrowed := owner;
    using var x := (case n of
      1 => new Res(1),
      _ => borrowed
    );
    return 0;
  end Main;
end W;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the arms of this if/case expression mix an owned value with a borrow
// CHECK-SAME: (§28.3, v0.184)
