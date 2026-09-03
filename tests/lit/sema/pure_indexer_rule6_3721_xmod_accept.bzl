// RUN: %bezelc -c -o %t.lib.o %S/pure_indexer_rule6_3721_xmod_lib.bzl
// RUN: %bezelc -c -o %t.o %s --import %t.lib.o
// RUN: %bezelc -c %s --import %t.lib.o 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// NO-FALSE-REJECT control for pure_indexer_rule6_3721_xmod_reject.bzl: the
// imported `PureBag`'s indexer IS `pure get`-declared, so a `pure` body
// reading it across the artifact boundary must still compile clean.
module App3721Y;
import IdxLib3721;

public class Program
  public static pure method P(b: PureBag): int
    return b[3];
  end P;
  public static method Main(): int => 0;
end Program;

// NODIAG-NOT: error
// NODIAG-NOT: §
