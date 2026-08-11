// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3044 (§10.4, v0.242): "Every enum `E` with underlying type `U` carries
// one compiler-provided static member — no declaration is written, and NO USER
// DECLARATION MAY OVERRIDE OR OVERLOAD IT". Rejecting the declaration is what
// makes the member's stated contract — total, never raises, returns the
// declared constant or null — trustworthy at every call site.

module M;

enum Priority Low, Medium, High;
  public static method FromUnderlyingOrNull(value: int): Priority? => null;
end Priority;

class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum `Priority` may not declare a member named `FromUnderlyingOrNull`
// CHECK-SAME: (§10.4)
