// RUN: %bezelc -c -o %t.o %s
//
// §35.4 / §9.7 (v0.226, issue #2209): the SCOPE control — a borrow-bearing
// choice HELD IN A METHOD-LOCAL VAR (the source's scope-bound local) is NOT
// an instance-field storage position. The §9.7 instance-field bullet is
// scoped to storage that outlives the source; a stack-local does not
// outlive the stack frame, so the rejection (`MethodOwnership::checkBorrow-
// StoreEscape`, `Dest::Field`) does not apply — its target classifier
// (`MemberAccessExpr` / bare-field-identifier) is wrong for a plain
// `var o := Opt.Some(local);` declaration. The `MethodOwnership::isBorrow-
// BearingChoice` / `isBorrowBearingRecord` predicates are correctly
// applied — the rejection is scoped to a destination that can outlive the
// source — so a method-local binding of a borrow-bearing choice value is
// admitted (the source's `local` is also bound in the same frame, the
// borrow is bounded, and any attempted LATER storage into a class field
// is the field-store check's job). This pin prevents the new choice leg
// from widening to non-field positions — the over-reject failure mode.

module M; class Res public method Val(): int return 5; end Val; end Res; choice Opt Some(r: Res), None; end Opt; class C public static method Main(): int using var local := new Res(); var o := Opt.Some(local); case o of Some(var x) => do return x.Val(); end, None => do return 0; end end; end Main; end C;
