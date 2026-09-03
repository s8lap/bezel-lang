// RUN: %bezelc -c -o %t.lib.o %S/pure_operator_rule6_3610_xmod_lib.bzl
// RUN: %bezelc -c -o %t.o %s --import %t.lib.o
// RUN: %bezelc -c %s --import %t.lib.o 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// NO-FALSE-REJECT control for pure_operator_rule6_3610_xmod_reject.bzl: the
// imported `Vec::operator +` IS `pure`-declared, so a `pure` body calling it
// across the artifact boundary must still compile clean.
module App3610X2;
import OpLib3610;

public class Program
  public static pure method P(a: Vec, b: Vec): int
    return a + b;
  end P;
end Program;

// NODIAG-NOT: error
// NODIAG-NOT: §
