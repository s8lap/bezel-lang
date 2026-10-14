// RUN: %bezelc -c -o %t.o %s
//
// §35.4 / §9.7 (v0.226, issue #2209): the ACCEPT (non-over-rejection) control
// for the field-store CHOICE leg — the null-source form for a borrow-bearing
// choice is admitted at every enforcement site the fix touches, mirroring the
// #1540 record rule's `new RefRec(null)` exemption. Three accepted shapes,
// one per enforcement site:
//
//   1. `MethodOwnership::checkBorrowStoreEscape` (`Dest::Field`): a
//      parameter-typed `Opt := Opt.None` constructor body — payload-free
//      case accessor at the assignment site. (A `Box(x: Opt)` constructor
//      where `x` is a parameter is REJECTED, exactly as the record rule
//      rejects `Box(x: RefRec)`: the parameter has no null-source proof.
//      This is the #1540 regime-uniformity rule applied to choices.)
//   2. `MethodOwnership::checkFieldInitializer`: `v: Opt := Opt.None;` — a
//      payload-free case accessor at the initializer site.
//   3. `MethodOwnership::checkFieldInitializer` (alternative shape):
//      `v: Opt := Opt.Some(null);` — an explicit `null` for the borrow-
//      carrying payload at the initializer site.

module M; class Res public method Val(): int return 5; end Val; end Res; choice Opt Some(r: Res?), None; end Opt; class Box public v: Opt; public constructor() v := Opt.None; end constructor; end Box; class BoxInit public v: Opt := Opt.None; end BoxInit; class BoxNull public v: Opt := Opt.Some(null); end BoxNull; class C public static method Main(): int return 0; end Main; end C;
