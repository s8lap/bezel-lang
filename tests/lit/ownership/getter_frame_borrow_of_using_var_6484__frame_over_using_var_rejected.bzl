// fix #6484 / §28.3 (spec v0.338): a getter returned a FRAME carrying a borrow
// of a scope-bound local, and the return site admitted it.
//
// §28.3 verbatim, *Returning a deferred-activation frame*: "provided every
// borrow the returned frame carries is sourced from the method's own parameters
// or receiver (directly or through the receiver's fields, the receiver-sourced
// notion of §28.6) … A returned frame carrying a borrow of anything else — a
// scope-bound local, a `using var`, a temporary — is rejected at the return
// site, since the frame would outlive the source". (v0.338)
//
// #6404 made the getter return walk the returned BINDING back to its producer,
// because §28.6's rule is about the returned VALUE and an `IdentifierExpr`
// reads Borrowed. That peel asks whether the chain reaches a FRESH OWNED
// producer; this rule asks the orthogonal question — what BORROW does the frame
// CARRY. On this program the mint-announcing carve stops the #6404 peel (the
// return type is a §28.3 announcement-table row-2 head, so `MakeItems()` being
// a fresh owned producer is announced and not an §28.6 escape), the peel never
// reaches a producer, and the escape was admitted.
//
// Measured ACCEPTED before the fix at ba713eb9; AOT + ASan on that program:
// `heap-use-after-free`, freed by `bz$elemDispose`, allocated in `MakeItems`,
// called from `bz$prop$Items$get` — the caller reads the frame's elements after
// the getter's scope disposed them.
//
// The companion ACCEPT row is
// `getter_frame_borrow_of_using_var_6484__frame_over_receiver_field_accepted`:
// byte-identical body, FIELD root instead of a `using var` LOCAL root. The two
// must come out differently — the field is §28.3's admitted receiver-sourced
// case, the local is the rejected one.
// per §28.3, §28.6, §28.7, announcement table row 2, K2.2, K6.2, #6484

// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cell public Name: string := ""; end Cell; class Factory private owned _items: OwningArray<Cell>; public constructor() _items := new OwningArray<Cell>(); end constructor; private static method MakeItems(): owned OwningArray<Cell> return new OwningArray<Cell>(); end MakeItems; public property View: IEnumerable<Cell> get => do using var items2 := MakeItems(); var frame := items2.Where(lambda(c) => true); return frame; end; end View; end Factory; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a getter may not return a frame that borrows `items2` — `using var` `items2` is disposed at scope end, so the returned frame outlives its source
// CHECK-SAME: (§28.3 returning a deferred-activation frame)