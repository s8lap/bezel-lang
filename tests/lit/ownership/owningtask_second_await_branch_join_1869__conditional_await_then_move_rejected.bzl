// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §30.5 (Pattern B) / Appendix C.6 (v0.184) — issue #1869 (SB01-C2).
//
// The SOUNDNESS half of the branch-join await-spend slice. A plain owned
// `OwningTask<T>` local awaited on ONE arm of an `if` is moved-from on that arm
// (§30.5 line ~7399). A `move t` AFTER the join then moves out a task that was
// already spent on the awaited arm — a real double-consume (the moved-into owner
// would dispose an already-disposed task frame: a use-after-free / double-free).
// The join leaves `t` maybe-consumed, so the `move` is a compile error.
//
// FAIL-WITHOUT / PASS-WITH: pre-#1869 the await-consume was a plain `Disposed`,
// so the `Owned ⊓ Disposed` join washed out to plain Owned and `move t` compiled
// clean — laundering a spent task into a fresh owner. Marking the consume an
// await-SPEND (branch-join → maybe-consumed) makes `move t` the static error
// this CHECK pins; the pre-fix compiler accepted the move.

module M;

class Cell public constructor() end constructor; end Cell;
class Holder public constructor(t: OwningTask<Cell>) end constructor; end Holder;

class C
  public static async method MakeAsync(): OwningTask<Cell>
    return new Cell();
  end MakeAsync;

  public static async method RunTest(flag: bool): Task
    var t := C.MakeAsync();          // plain owned local OwningTask<Cell>
    if flag then
      using var g := await t;        // consumes (spends) t on this arm only
    end;
    var h := new Holder(move t);     // move of a maybe-spent task — rejected
  end RunTest;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot consume `t`: it may have already been consumed on another branch
// CHECK-SAME: (maybe-consumed join, §28.3)
