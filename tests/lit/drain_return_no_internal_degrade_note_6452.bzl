// fix #6452 (codegen; language spec §28.3 *Returning a deferred-activation
// frame* / §29.2 *Desugaring of `for`* / §29.1 *`Current` semantics on
// borrowing and owning enumerators*; stdlib §4.5 / §4.13, v0.338): the
// `return recv.Drain();` edge produced a NULL return value behind a CLEAN
// compile.
//
// THIS FILE IS THE DIAGNOSTIC ORACLE, and it has to be lit rather than gtest
// for the reason its #3434 sibling records: the AOT gtest harness discards
// captured compiler stderr on a SUCCESSFUL compile (tests/support/
// aot_support.hpp), so an "assert no internal note" written there passes
// VACUOUSLY on any compiler, fixed or not. This file's whole content IS the
// diagnostic — the `internal: … #2491` note the fix removes — so it cannot
// live in gtest. The RUNTIME half (the program used to SEGV at the call site)
// is gtest's: tests/codegen/drain_view_return_frame_lowered_6452_test.cpp.
//
// WHAT WAS WRONG. `lowerCallExpr` has no `Drain` arm — only the for-in
// desugaring and the GetOwningEnumerator dispatch recognise the shape, and
// both do it by walking BACK to the local-decl initializer. At a RETURN there
// is no initializer, so the call lowered to nothing, `v` stayed null, the §29.2
// return coercion was gated on `v && rs->value` and never ran, and
// `emitReturn`'s `v ? v : null` fallback handed the caller a null frame:
//
//     warning: internal: return-value expression failed to lower to a value
//     (degraded to a zero/null return value) — #2491
//
// then `AddressSanitizer: SEGV on unknown address 0x000000000000` in the
// CALLER. The note is a **Warning** on purpose — the §3 / #605 permissive
// posture keeps it one, because §3's `return o.Anything();` over an `Object`
// receiver reaches the same site legally — which is why a clean compile meant
// nothing and the program crashed in user code.
//
// WHY A REAL VALUE AND NOT A DIAGNOSTIC. §28.3: "A non-iterator method may
// therefore compose and return such a frame ... provided every borrow the
// returned frame carries is sourced from the method's own parameters or
// receiver (directly or through the receiver's fields, the receiver-sourced
// notion of §28.6)." The three shapes below are exactly that receiver-sourced
// case, and gtest's
// `drain_view_return_site_2175.return_this_field_drain_admitted_control_
// spec_28_3` pins the first one as ADMITTED (`hasErrors() == false`). Rejecting
// them would contradict a pinned admission.
//
// FOUR INDEPENDENT ROUTES, so this cannot pass because one arm was
// special-cased. They differ in the HEAD the drain is taken over, in the return
// TYPE, and in how the value reaches the caller:
//   * `OwningArray`  — the forward-array leaf (`owningDrainWrapForHead`'s
//     `ArrayForward`), returned through a PLAIN METHOD, bound with `using var`.
//   * `OwningStack`  — the LIFO leaf (`ArrayLifo`), same method face.
//   * `OwningLinkedList`   — the node-list leaf (`List`).
//   * a `using var` PARAMETER — §28.3's other admitted source ("the method's
//     own parameters"), a different receiver class from the three above.
//
// The NEGATIVE controls live in the same directory:
//   * `drain_return_local_bound_still_no_degrade_note_6452.bzl` — the
//     `var v := recv.Drain(); return v;` WORKAROUND, which already lowered
//     correctly. It must keep being silent, so a fix that merely suppressed the
//     note would go green here and red there. This is what makes the file a
//     gate on the VALUE rather than on the text.
//   * `drain_return_owned_parameter_still_degrades_6452.bzl` — the receiver
//     class this fix deliberately DECLINES, because an `owned` parameter is
//     discharged at the callee's scope end and a frame minted over it dangles.
//     It still draws the `#2491` note, in both spellings. This is what makes
//     "no `#2491` on this arm" a claim about a SCOPE rather than about the note
//     being suppressed wholesale — and it is the row that stops the scope from
//     widening silently, since an arm that admitted this class would start
//     minting here and the CHECKs below would stop matching.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
import System.Console;

class R
  public static method Live(): int => 0;
  public constructor()
  end constructor;

  public method Tag(): string => "r";
end R;

class Bag
  // (1) forward-array backing — `ArrayForward`
  public owned arr: OwningArray<R>;

  // (2) LIFO backing — `ArrayLifo`
  public owned stk: OwningStack<R>;

  // (3) node-list backing — `List`
  public owned lst: OwningLinkedList<R>;

  public constructor()
    arr := new OwningArray<R>();
    stk := new OwningStack<R>();
    lst := new OwningLinkedList<R>();
    arr.Add(new R());
    arr.Add(new R());
    stk.Push(new R());
    stk.Push(new R());
    lst.Add(new R());
    lst.Add(new R());
  end constructor;

  public method FromArray(): IOwningEnumerable<R>
    return arr.Drain();
  end FromArray;

  public method FromStack(): IOwningEnumerable<R>
    return stk.Drain();
  end FromStack;

  public method FromList(): IOwningEnumerable<R>
    return lst.Drain();
  end FromList;
end Bag;

class C
  // (4) §28.3's other admitted source: a PLAIN PARAMETER, whose lifetime is
  // wider than the call — the shape
  // `drain_view_return_site_2175.return_parameter_drain_admitted_control_
  // spec_28_3` pins as admitted. A different receiver class from the three
  // above: not a field read, not a fresh producer.
  public static method FromParam(items: OwningArray<R>): IOwningEnumerable<R>
    return items.Drain();
  end FromParam;

  public static method Main(): int
    using var b := new Bag();
    var n := 0;
    using var a: IOwningEnumerable<R> := b.FromArray();
    for var r in a do
      n := n + 1;
    end for;
    using var s: IOwningEnumerable<R> := b.FromStack();
    for var r in s do
      n := n + 1;
    end for;
    using var l: IOwningEnumerable<R> := b.FromList();
    for var r in l do
      n := n + 1;
    end for;
    using var q: IOwningEnumerable<R> := C.FromParam(b.arr);
    for var r in q do
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