// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the compile is expected to fail —
// see tests/lit/README.md for why every reject test wraps `bezelc` in `not`.)
//
// Issue #6729 / spec §12.2 (Hiding is not allowed), §11.7 (Static classes),
// §8.2 (`GetType`), v0.340. §12.2: "`GetType` and `ReferenceEquals` block as
// every other inherited member does"; `GetType` is non-virtual, and
// `ReferenceEquals` is static, so no `override` reaches either. §11.7: a static
// class's static `ReferenceEquals` is blocked (`Object`'s static is inherited by
// every class), and its static `GetType` is admitted (the accept side is pinned
// in tests/sema/object_gettype_refeq_hiding_6729_test.cpp).
//
// The diagnostic is at each member's declaration, one per offending member.
module M;

public static class U
  public static method ReferenceEquals(a: Object?, b: Object?): bool => true;
end U;

public class K
  public method GetType(): int => 1;
end K;

public class K2
  public static method GetType(): int => 1;
end K2;

public class K3
  public GetType: int := 0;
end K3;

public class Widget
  public override method GetType(): Type => typeof(Widget);
end Widget;

// CHECK: :18:3: static member `ReferenceEquals` hides the inherited `Object.ReferenceEquals`
// CHECK-SAME: (§11.7, §12.2)
// CHECK: :22:3: member `GetType` hides the inherited `Object.GetType`
// CHECK-SAME: `GetType` is non-virtual
// CHECK: :26:3: static member `GetType` hides the inherited `Object.GetType`
// CHECK: :30:3: field `GetType` hides the inherited `Object.GetType`
// CHECK: :34:3: member `GetType` hides the inherited `Object.GetType`
