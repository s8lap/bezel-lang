// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §28.3 arm-meet (v0.184/v0.196; issue #1661): the ownership of an if-expression
// `v1 if c else v2` is the MEET of its arms — owned⊓owned=owned,
// borrowed⊓borrowed=borrowed — so a MIXED owned/borrowed arm set has no coherent
// owner and must be REJECTED (spec §28.3 producers row; §20). Here the `then`
// arm is a fresh owned producer (`new Res(1)`) and the `else` arm is a borrow
// (`borrowed`, a local aliasing the owned `owner`). Before the fix this was
// SILENTLY ACCEPTED and the fresh `new Res(1)` leaked (the whole expression was
// misclassified a borrow, so no disposal obligation was assumed) — see the
// runAot LSan sibling arm_meet_ownership_1661_test.cpp.
//
// Fails-without / passes-with: the reject is emitted by MethodOwnership::
// checkArmMeetMixed (Ownership.cpp), added by the #1661 fix; without it the
// program compiles clean (no diagnostic) and FileCheck fails to match.

module M;
class Res : IDisposable
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
  destructor end destructor;
end Res;
class W
  public static method Main(): int
    var c := true;
    using var owner := new Res(9);
    var borrowed := owner;
    using var x := (if c then new Res(1) else borrowed);
    return 0;
  end Main;
end W;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the arms of this if/case expression mix an owned value with a borrow
// CHECK-SAME: (§28.3, v0.184)
