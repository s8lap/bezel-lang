// RUN: %bezelc -c -o %t.lib.o %S/imported_literal_target_false_reject_5328rv__lib.bzl
// RUN: %bezelc -c %s --import %t.lib.o
// RUN: %bezelc -c %s --import %t.lib.o 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// Review row for issue #5328 / spec §15.4 (v0.313) — FALSE-REJECT PROBE 3 of
// 5: a `Queue<int>` formal, imported, with a sequence literal.
//
// §15.4 puts "`Queue<T>` and `OwningQueue<T>` (FIFO queues)" on the closed
// target list, so `Que([1, 2, 3])` at `Que(q: Queue<int>)` is well-typed and
// must compile clean.
//
// Together with probes 1 and 2 this row covers three DIFFERENT builtin
// collection heads (`Array`, `OwningSet`, `Queue`) admitted across the module
// boundary by the same predicate. That breadth is the point: the gate decides
// by SIMPLE NAME, so the evidence that it does not over-reject is that it does
// not over-reject for several names, not for one.
module FRP3;

import FR5328Lib;

public class Program
  public static method Main(): int
    Console.WriteLine(FR5328Lib.FROps.Que([1, 2, 3]));
    return 0;
  end Main;
end Program;

// NODIAG-NOT: error
// NODIAG-NOT: cannot target
// NODIAG-NOT: §15.4
// NODIAG-NOT: internal:
