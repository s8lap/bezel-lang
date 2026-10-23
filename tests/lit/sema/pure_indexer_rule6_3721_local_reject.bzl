// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Diagnostic-track pin for the INDEXER twin of §22.15 rule 6's
// accessor-read arm (v0.286), ported from
// tests/module/xmod_rule6_indexer_accessor_read_arm_3721_test.cpp's
// `non_pure_indexer_getter_in_pure_body_rejected_3721` — the issue's own
// measured repro. `Bag`'s indexer `get` is not `pure`-declared and writes
// a static field; `P` is `pure` and reads `b[3]`. Pre-fix this compiled
// with exit 0 and the executable exited 4 — the impure getter's write ran
// inside the pure body.
//
// #6620: `internal:` is codegen-only and cannot occur on a row that
// rejects in the front end; this needle is the front end's own ICE marker.
// CHECK-NOT: internal compiler error

module App;
public class Bag
  public static Calls: int := 0;
  public property [i: int]: int
    get => do Calls := Calls + 1; return i + Calls; end;
  end;
end Bag;
public class Program
  public static pure method P(b: Bag): int
    return b[3];
  end P;
  public static method Main(): int => 0;
end Program;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: indexer read is not admitted in a pure body
// CHECK-SAME: (§22.15)
