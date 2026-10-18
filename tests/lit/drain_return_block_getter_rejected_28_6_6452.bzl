// fix #6452 — the REJECTION CONTROL for
// `drain_return_no_internal_degrade_note_6452.bzl`, in the same directory.
//
// #6431's review rejected its own commit (`6073bc0e`) over exactly what this
// file pins: the §28.6 carve that admits a receiver-sourced drain at a
// mint-announcing RETURN was widened to the BLOCK-bodied getter face, and that
// face was — in the reviewer's measured words — "UNPINNED: no gtest or lit row
// covers a block-form getter at a mint-announcing head returning a
// deferred-activation frame, and the nearest row is the EXPRESSION form,
// untouched". So removing that rejection moved no row, and green gtest, green
// lit and green corpus were all correct and all irrelevant.
//
// This file is that missing row. It is lit rather than gtest because the row
// is about a NEEDLE: the §28.6 diagnostic's exact wording is what a future
// carve has to re-examine, and the AOT gtest harness discards captured
// compiler stderr on a successful compile.
//
// The pairing is with the expression-form sibling, which
// `drain_view_getter_2175.getter_drain_over_this_field_rejected_spec_28_6`
// pins in gtest — same rule, same needle, the two BODY FORMS of §23.3's
// "the `get => e` and `get => do … end` spellings are semantically
// indistinguishable at every read site". Between them the two rows cover both
// spellings.
//
// WHY IT IS STILL REJECTED AFTER #6452. §28.6 (v0.338): "a property or
// indexer getter may not return a freshly-produced owned value — a getter is a
// borrow-producing position", and §28.3's frame rule lifts only the CALLEE-side
// borrow-return prohibition, leaving §28.6's owned-escape rules "unaffected
// for values outside the families". `Drain()` "mints a fresh drain-view
// frame" (stdlib §4.13), which is a freshly-produced owned value. #6452 gave
// the METHOD face a real lowering; admitting this shape would need a §28.6
// carve, and a carve is a spec ruling, not a codegen change.
//
// WITH the carve it would not merely work: before #6452 it compiled clean,
// warned only internally, and then SEGV'd at the caller. That is the
// failure mode this row exists to keep off the table until the lowering a
// carve would rely on is measured.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck --check-prefix=REJECT %s

module M;

class R
  public constructor()
  end constructor;
end R;

class Holder
  private owned _items: OwningArray<R>;

  public constructor()
    _items := new OwningArray<R>();
    _items.Add(new R());
  end constructor;

  // The BLOCK-bodied getter at a mint-announcing head returning a
  // deferred-activation frame. `get => do … end` is §23.3's second spelling;
  // the expression-bodied sibling is gtest's
  // `drain_view_getter_2175.getter_drain_over_this_field_rejected_spec_28_6`.
  public property Q: IOwningEnumerable<R>
    get => do
      return _items.Drain();
    end;
  end Q;
end Holder;

class P
  public static method Run(): int
    using var h := new Holder();
    using var view: IOwningEnumerable<R> := h.Q;
    var n := 0;
    for var r in view do
      n := n + 1;
    end for;
    return n;
  end Run;
end P;

// REJECT: a property or indexer getter may not return a freshly-produced
// REJECT: borrow-producing position
// REJECT: §28.6
// NOFRAME: #2491