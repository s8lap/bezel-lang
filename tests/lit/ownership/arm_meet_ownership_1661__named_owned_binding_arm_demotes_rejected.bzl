// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §28.3 arm-meet (v0.184/v0.196; issue #1661): "A named owned binding written as
// an arm is an unmarked read and therefore a BORROW arm (there is no arm-position
// transfer marker)." So `new Res(1) if c else held` — where `held` is an owned
// local — is an owned arm meeting a BORROW arm (the demoted named binding): a
// MIXED set, REJECTED. This pins the demotion rule specifically: `held` is
// genuinely owned, yet as an arm it reads as a borrow, so the meet is mixed
// rather than owned⊓owned.
//
// Fails-without / passes-with: producerState classifies a bare owned-local read
// as Borrowed (the local-read row), so checkArmMeetMixed sees owned+borrow and
// rejects; without the #1661 fix the program compiled clean.

module M;
class Res : IDisposable
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
  destructor end destructor;
end Res;
class W
  public static method Main(): int
    var c := true;
    using var held := new Res(2);
    using var x := new Res(1) if c else held;
    return 0;
  end Main;
end W;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the arms of this if/case expression mix an owned value with a borrow
// CHECK-SAME: a named owned binding written as an arm is a borrow arm
