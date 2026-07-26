// RUN: %bezelc -c -o %t.o %s
//
// §35.4 / §9.7 (v0.226, issue #2209): the ACCEPT (non-over-rejection) control
// for the field-store CHOICE leg — a VALUE-LIKE choice (no cases carry
// borrow-typed payloads; §35.4.3: every payload is value-typed or runtime-
// managed) is not borrow-bearing per `MethodOwnership::isBorrowBearingChoice`'s
// single §35.4 authority, so storing it into a class instance field is
// ordinary value-storage and must compile cleanly. The new `isBorrowBearingChoice`
// gate on the choice leg must mirror exactly the gate the existing
// `isBorrowBearingRecord` predicates apply to record values — and the
// `common::classifyChoiceType` authority it wraps must classify this choice
// as `ValueLike`, NOT `BorrowBearing`.

module M; choice VOpt Some(n: int), Named(s: string), None; end VOpt; class Box public v: VOpt; public constructor(x: VOpt) v := x; end constructor; end Box; class C public static method Main(): int return 0; end Main; end C;
