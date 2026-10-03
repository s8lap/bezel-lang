// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #5328 / spec §15.4 (v0.313) — CONTROL ROW for PIN ROW 1: the
// SAME-MODULE twin of `imported_literal_target_5328__iface_imported_rejected.bzl`.
//
// The fix teaches the IMPORTED channel a check the local channel has run
// since #1174. A change that widened WHAT the local channel rejects would
// show up here as a flipped expectation; this row exists so the local leg is
// pinned to the diagnostic it has always produced, byte for byte, and so the
// two rows are comparable by eye: same rule, same wording, same phase, at
// the same call-argument location. §2.6 requires the cross-module twin of a
// local rule to be the local rule; that is only checkable if both are pinned.
module App5328IfaceLocal;

public class Ops
  public static method SumE(x: IEnumerable<int>): int
    var s := 0;
    for var v in x do
      s := s + v;
    end for;
    return s;
  end SumE;
end Ops;

public class Program
  public static method Main(): int
    Console.WriteLine(Ops.SumE([1, 2, 3]));
    return 0;
  end Main;
end Program;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a collection literal `[...]` cannot target the interface type `IEnumerable`
// CHECK-SAME: §15.4 admits only a concrete collection target
// CHECK-SAME: (§15.4)
// CHECK-NOT: internal:
