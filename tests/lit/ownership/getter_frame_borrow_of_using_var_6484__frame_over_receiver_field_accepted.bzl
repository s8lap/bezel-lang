// fix #6484 — the ACCEPT half of
// `getter_frame_borrow_of_using_var_6484__frame_over_using_var_rejected.bzl`,
// and the over-rejection control for that rejection.
//
// §28.6 bullet 2's mint-announcing parenthetical and §28.3's corresponding
// exception admit returning a frame whose borrow is receiver-sourced. This file
// is that shape: the deferred-activation frame is built over the getter's OWN
// `owned` FIELD and bound to a local before the return — the SAME laundering
// spelling the sibling rejects, with the root changed from a `using var` LOCAL
// to a receiver FIELD.
//
// That is the boundary the fix draws, and it is the one the issue names ("a fix
// here must reject a scope-bound local while continuing to accept the field").
// It is also the shape three landed commits rest on: `d62a8a4a` (#6404),
// `83cd1b79`, and `05ac79a1` (#6452, whose accepted row is
// `drain_return_local_bound_still_no_degrade_note_6452.bzl`).
//
// So this row is load-bearing in the direction that matters most for a fix that
// ADDS a rejection: without it, a check that simply rejected every
// getter-returned frame would leave the sibling reject-row green while turning
// a legal program into a false rejection.
//
// MEASURED ACCEPTED before the fix at ba713eb9 and still ACCEPTED after it.
// per §28.3, §28.6, §28.7, announcement table row 2, #6484

// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell public Name: string := ""; end Cell; class Factory private owned _items: OwningArray<Cell>; public constructor() _items := new OwningArray<Cell>(); end constructor; public property View: IOwningEnumerable<Cell> get => do var v := _items.Drain(); return v; end; end View; end Factory; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §