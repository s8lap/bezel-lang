// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
// RUN: %not %bezelc -c %s 2>&1 | grep -c "parameter disposed at the end of this method" | FileCheck --check-prefix=TWICE %s
// RUN: %not %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

// fix #6462 — the `owned`-PARAMETER receiver class of the #2175(a) return-site
// drain check, REJECTED. This file REPLACES `drain_return_owned_parameter_-
// still_degrades_6452.bzl` (git-mv, content rewritten): that row pinned the
// `#2491` degraded null return as the interim state and its own header said
// "whoever fixes the ownership pass replaces this `warn` case with the `run`
// twin ... rather than editing the CHECK lines below". This is that
// replacement. Its sibling reference in `drain_return_no_internal_degrade_-
// note_6452.bzl` is updated in the same commit.
//
// THE RULE. §28.3 *Borrow lifetime rules*, in the FIELD-STORE bullet's
// clause 2 (the bullet opening "A borrow may be stored in a field of an object
// only if..."), states the classification outright: "an `owned` parameter is
// scope-bound: the method disposes its undischarged value at its own end
// (*Auto-drop (affine)* below), so a borrow of it is rejected here exactly as
// a borrow of a scope-bound local is." (That is the field-store bullet's
// address, NOT the section's separate local-scope bullet — the "assigned to a
// local whose scope strictly encloses" rule — but the quoted words are what
// carry the verdict.) *Owned-local discharge requirement*'s *Auto-drop
// (affine)* puts that discharge at "the end of the method". So the parameter
// does NOT outlive the call — the premise every admitted parameter-sourced
// frame rests on — and §28.3 *Returning a deferred-activation frame* rejects at
// the return site "a borrow of anything else — a scope-bound local, a `using
// var`, a temporary ... since the frame would outlive the source". Its reject
// list is keyed on SCOPE-BOUND, and this class is.
//
// THE SITE. The discharge is at function exit, but the RULE is still at the
// return site — §28.3 says "rejected at the return site" in three places
// (*Returning a deferred-activation frame*, *Borrow lifetime rules* item 4,
// and the bare-type-parameter callee side's item 4). The discharge's TIMING is
// what makes the parameter scope-bound; it is not a different site to check at.
// §22.3 writes the classification on the formal, and it is `owned` ALONE —
// `out owned` is a PRODUCER (§22.3: "ownership reaches the caller only on
// normal return"), whose value is transferred rather than disposed at method
// end, so it outlives the call and stays admitted. That is the predicate both
// arms of the check read.
//
// BOTH SPELLINGS ARE HERE, and the second RUN line asserts the EXACT COUNT of
// the diagnostic (two, one per spelling), because a narrow that asked only
// about the DIRECT receiver would reject the direct spelling and admit the
// chain root — the same partial-narrowing failure #6452's reviewer named, and
// the same one the ownership pass's chain-root walk exists to prevent. `grep
// -c` prints the count of matching lines and the pattern demands exactly `2`,
// so a regression to one drops the count and this file goes red. A single
// FileCheck directive would not: it matches the FIRST occurrence and moves
// past it. (FileCheck's own count-taking directive form is not a minimum
// either — measured against 0, 1, 2 and 3 occurrences it fails only at 0 — so
// it means "match once, don't insist on more" and would sail through a
// harmless rewording of the message. The `grep -c` exact count above is the
// property this row actually needs.)
//
// The third RUN line is the NEGATIVE half: the `#2491` degrade note must be
// GONE. A positive-only row would go green if the file failed for an
// unrelated reason while the degrade note survived; this says the reject is
// the ownership pass's verdict and not the codegen arm's fallback. The
// codegen narrow (`Emitter::drainReceiverSourceDiesAtScopeEnd`) is UNCHANGED —
// it is neither widened nor narrowed further, and a future edit that made the
// ownership pass stop rejecting would leave it still declining to mint, so
// these lines together are what holds the pair honest.
//
// It is lit rather than gtest because the content is a NEEDLE: the gtest AOT
// harness discards captured compiler stderr on a successful compile, so an
// "assert the diagnostic" row there would pass vacuously.

module M;

class R
  public constructor()
  end constructor;

  public method Tag(): string
    return "r";
  end Tag;
end R;

class Bag
  public owned items: OwningArray<R>;

  public constructor()
    items := new OwningArray<R>();
    items.Add(new R());
  end constructor;
end Bag;

class Src
  public static method Make(): owned OwningArray<R>
    var a := new OwningArray<R>();
    a.Add(new R());
    return a;
  end Make;
end Src;

// The DIRECT spelling: the drain receiver IS the `owned` parameter.
// CHECK: cannot return `items`.Drain()
// CHECK-SAME: an `owned` parameter disposed at the end of this method
class C
  public static method View(owned items: OwningArray<R>): IOwningEnumerable<R>
    return items.Drain();
  end View;

  public static method Main(): int
    var w := Src.Make();
    using var view: IOwningEnumerable<R> := C.View(move w);
    var n := 0;
    for var r in view do
      n := n + 1;
    end for;
    Console.WriteLine($"{n}");
    return 0;
  end Main;
end C;

// The CHAIN spelling: the drain receiver is an ordinary field, and the ROOT of
// the chain is the `owned` parameter. Both arms of the check walk to the root,
// which is what makes the second occurrence below happen at all.
class D
  public static method View(owned p: Bag): IOwningEnumerable<R>
    return p.items.Drain();
  end View;

  public static method Main(): int
    var b := new Bag();
    using var view: IOwningEnumerable<R> := D.View(move b);
    var n := 0;
    for var r in view do
      n := n + 1;
    end for;
    Console.WriteLine($"{n}");
    return 0;
  end Main;
end D;

// TWICE: {{^2$}}
//
// ONE occurrence, for the DIRECT spelling (class C); the second is pinned by
// the exact-count RUN line above. Note that FileCheck reads a directive from
// any comment line carrying the prefix, so prose in this file must never QUOTE
// a directive verbatim — an earlier draft of its #6452 predecessor did, and
// the phantom directive consumed a real occurrence and turned the row red for
// a reason that had nothing to do with the change under test.
//
// NODIAG-NOT: #2491
//
// The `#2491` internal degrade note must be ABSENT from this file's compile.
// Its presence would mean the ownership pass admitted the class and codegen's
// narrow caught it instead — which is the interim state this row replaces, and
// which leaves the program crashing at a null page rather than rejected. It is
// asserted as an absence rather than as a `grep -c` zero, because `grep -c`
// exits 1 on no match and lit's shell reports that pipeline stage's status
// before FileCheck ever sees the `0` — the row would then fail for the wrong
// reason, or pass for none.