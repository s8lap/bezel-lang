// Regression pin for #7487 (§28.10 adoption + §28.12 scope-end discharge +
// §28.3 / §30.1 frame teardown, v0.350) — the ASYNC spelling of the #7415 fix.
//
// An `owned` move-only parameter of an `async` method that the body never
// discharges is disposed by the frame at teardown (§30.1), and §28.3's
// must-use rule attaches the warning "owned parameter `X` is implicitly
// disposed by the frame at teardown" to it. When the body instead COPIES the
// parameter into an `await using var` binding, §28.10 makes that copy a MOVE:
// the binding carries the value's owned slots, the PARAMETER is left
// moved-from, and the frame has nothing left to dispose — so the warning must
// be ABSENT for that parameter. (Before #7415 the owning-choice parameter was
// not seen as moved by the copy, the frame flag stayed set, and the warning
// fired on a parameter the program had in fact discharged.)
//
// `ignored` is the DIFFERENTIAL control in the same file: it is the same type,
// never discharged, and MUST still warn — so the CHECK-NOT below cannot pass
// merely because the warning stopped being emitted at all. The two methods are
// emitted in source order, `copied` first, which the CHECK-NOT / CHECK pair
// relies on.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M;
class Res
  public constructor() end constructor;
  destructor end destructor;
end Res;
choice OChoice Text(owned r: Res), Empty; end OChoice;
class P
  // `await using var` copy of the owned parameter: a MOVE, so no frame warning.
  public static async method Copied(owned copied: OChoice): Task
    await using var x := copied;
  end Copied;
  // Never discharged: the frame disposes it and the must-use warning fires.
  public static async method Ignored(owned ignored: OChoice): Task
  end Ignored;
end P;

// CHECK-NOT: owned parameter `copied` is implicitly disposed
// CHECK: owned parameter `ignored` is implicitly disposed by the frame at teardown (§28.3/§29.8/§30.1 must-use / auto-drop)
// CHECK-NOT: owned parameter `copied` is implicitly disposed
