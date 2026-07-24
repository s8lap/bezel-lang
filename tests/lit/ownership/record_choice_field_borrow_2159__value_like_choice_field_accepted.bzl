// RUN: %bezelc -c -o %t.o %s
//
// §35.4.3 / §9.7 (v0.226, issue #2159): the ACCEPT (non-over-rejection) control.
// A record whose choice field is VALUE-LIKE (`VOpt` has only `int`/`string`
// payloads, §35.4.3) is NOT a borrow-bearing record — the choice carries no
// borrow — so returning it is legal. The new Choice leg must consult
// classifyChoiceType and gate on BorrowBearing, not widen to "any choice
// field"; a value-like choice field must leave the record value-like.

module M; choice VOpt Some(n: int), Named(s: string), None; end VOpt; record HolderV(K: VOpt); class C public static method Make(): HolderV return new HolderV(VOpt.Some(7)); end Make; public static method Main(): int var h := Make(); return 0; end Main; end C;
