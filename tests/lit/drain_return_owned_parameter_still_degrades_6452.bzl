// fix #6452 — the SCOPE CONTROL for
// `drain_return_no_internal_degrade_note_6452.bzl`, in the same directory, and
// the row that keeps a DELIBERATE non-fix from becoming a silent one.
//
// WHAT IT PINS. `return recv.Drain();` where the receiver is an `owned`
// PARAMETER must still draw the `internal: … #2491` note — i.e. must still
// take the pre-#6452 path (a null return, and a null-page SEGV at the caller).
// It is a `warn` case, not an `accept`, because that note is the whole content.
//
// WHY IT IS A DELIBERATE NON-FIX. §28.3 (v0.338) lists the sources a returned
// frame may not carry, and an `owned` parameter is one of them: "a
// scope-bound local, a `using var`, an `owned` parameter". §28.3's affine
// auto-drop discharges that parameter at the method's own scope end, so a
// frame minted over it dangles.
//
// The ownership pass does not reject it. `Ownership.cpp`'s
// `cannot return ….Drain()` site admits every UNTRACKED receiver, because a
// plain parameter outlives the call — its two arms both end at
// "untracked (parameter / import) — admitted". An `owned` parameter is
// untracked in exactly the same sense, so it is admitted too, and it reaches
// codegen. That gap is a real missing rule and is being filed separately.
//
// MEASURED, both legs, one program per spelling:
//
//   View(owned items: OwningArray<R>) return items.Drain();   // direct
//   View(owned p: Bag)                 return p.items.Drain(); // chain root
//
//   base  edf42bd3              compiles clean, `#2491`, SEGV at 0x0
//   #6452 without the narrowing compiles clean, SEGV inside
//                              `bz_array_drain_next`
//                              (src/runtime/bezel_runtime_collections.cpp)
//                              on a small NON-ZERO address — 0x0a for this
//                              spelling, 0x0b for the chain root on the same
//                              run — a frame over a collection the callee had
//                              already freed. The address is NOT a stable fact
//                              (it is a wild read at a small offset off the
//                              freed array); the function and the UAF class
//                              are.
//   #6452 WITH the narrowing  compiles clean, `#2491`, SEGV at 0x0  ← this file
//
// A null-page crash is a bad failure mode. A use-after-free is a worse one,
// and the difference is worth a `#2491` note the user can grep for. So the
// arm declines the mint here, and this row is what makes the decline LOUD: an
// arm that widened past `owned` parameters without noticing would turn this
// file green-to-red only in the sense that it would start minting — and the
// `#2491` CHECK below is exactly what would stop it.
//
// ⚠ THIS ROW GOES RED THE MOMENT THE REAL RULE LANDS, and that is correct: an
// `owned`-parameter drain admitted AND correctly lowered must stop degrading.
// Whoever fixes the ownership pass replaces this `warn` case with the `run`
// twin of `s28_3_0203`, rather than editing the CHECK lines below.
//
// It is lit rather than gtest because the content is a NEEDLE — the gtest AOT
// harness discards captured compiler stderr on a successful compile, so an
// "assert the note" row there would pass vacuously.
//
// THE COUNT IS ASSERTED, NOT JUST THE PRESENCE. One FileCheck directive
// matches the FIRST occurrence and then moves past it, so the single presence
// directive at the foot of this file says nothing about the second spelling: a
// narrowing that handled the direct receiver and missed the chain root would
// emit ONE note and still pass. The two `TWICE` RUN lines below close that
// hole with an EXACT count — `grep -c` prints the number of matching lines and
// the pattern below demands exactly two, so a widening that mints over the
// chain root drops the count to 1 and this file goes red.
//
// (FileCheck's own count directive would NOT do this, and the reason is worth
// recording: its `CHECK-COUNT-<n>` form is not a minimum either. Measured here
// against 0, 1, 2 and 3 occurrences of the needle, it fails only at 0 and
// passes at every count above it — it means "match once, don't insist on
// more". A harmless rewording of the `#2491` message would sail straight
// through it, which is exactly the hole this file must not have.)
//
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=DEGRADES %s
// RUN: %bezelc -c %s 2>&1 | grep -c "#2491" | FileCheck --check-prefix=TWICE %s
// RUN: %bezelc -c %s 2>&1 | grep -c "failed to lower to a value" | FileCheck --check-prefix=TWICE %s

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

// The DIRECT spelling: the drain receiver IS the owned parameter.
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

// The CHAIN spelling: the drain receiver is a field, and the ROOT is the owned
// parameter. A narrowing that asks only the direct receiver misses this one,
// and the two are the same hazard — which is why Ownership.cpp walks the chain
// for the same reason.
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

// DEGRADES: #2491
// DEGRADES: failed to lower to a value
//
// ONE occurrence of each needle, for the DIRECT spelling (class C); the second
// occurrence of each is pinned by the `TWICE` count assertions in the RUN block
// above, which is what makes "both spellings still degrade" a claim the row
// actually HOLDS rather than one it merely states.
//
// TWICE: {{^2$}}
//
// (The exact-count pattern lives here, and the `DEGRADES` directives above it
// match only the first occurrence. Note that FileCheck reads a directive from
// any comment line that carries the prefix, so prose in this file must never
// quote a directive verbatim — an earlier draft of this comment did, and the
// phantom directive consumed a real occurrence and turned the row red for a
// reason that had nothing to do with the narrowing.)