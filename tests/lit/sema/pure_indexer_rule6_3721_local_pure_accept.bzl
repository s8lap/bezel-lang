// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// NO-FALSE-REJECT control for pure_indexer_rule6_3721_local_reject.bzl: the
// character-identical twin with `pure get` on the indexer accessor must
// still compile — a fix that rejected every indexer read regardless of
// purity would fail this row while passing the reject row.

module App;
public class Bag
  public property [i: int]: int
    pure get => i + 1;
  end;
end Bag;
public class Program
  public static pure method P(b: Bag): int
    return b[3];
  end P;
  public static method Main(): int => 0;
end Program;

// NODIAG-NOT: error
// NODIAG-NOT: §
