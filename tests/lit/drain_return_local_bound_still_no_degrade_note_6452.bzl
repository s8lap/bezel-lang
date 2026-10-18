// fix #6452 — the NEGATIVE CONTROL for
// `drain_return_no_internal_degrade_note_6452.bzl`, in the same directory.
//
// The fix there mints a §29.2 wrapper frame at the `return recv.Drain();` edge
// so the value is no longer null. The risk that control exists to catch is a
// fix that SUPPRESSES the `#2491` note instead of producing a value: such a
// fix turns every route in the sibling green, and this file — the shape
// #6452's issue records as the WORKING WORKAROUND ("bind to a local first …
// that path lowers correctly and returns the right answer") — is where the
// difference shows, because a note-suppressing fix has no reason to leave the
// already-correct shape silent.
//
// So both files must be silent, and only one of them can be silent by
// construction. Together they turn "no `#2491` on this arm" from a claim about
// a text into a claim about a VALUE.
//
// Spec: §28.3 *Returning a deferred-activation frame* (v0.338) — the
// receiver-sourced borrow of §28.6 travels through the local unchanged, so
// `return v;` re-announces the same frame the direct edge mints.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;

class R
  public constructor()
  end constructor;

  public method Tag(): string => "r";
end R;

class Bag
  public owned arr: OwningArray<R>;

  public constructor()
    arr := new OwningArray<R>();
    arr.Add(new R());
    arr.Add(new R());
  end constructor;

  public method View(): IOwningEnumerable<R>
    var v := arr.Drain();
    return v;
  end View;
end Bag;

class C
  public static method Main(): int
    using var b := new Bag();
    var n := 0;
    using var view: IOwningEnumerable<R> := b.View();
    for var r in view do
      n := n + 1;
    end for;
    Console.WriteLine(n);
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: internal:
// NODIAG-NOT: #2491
// NODIAG-NOT: failed to lower