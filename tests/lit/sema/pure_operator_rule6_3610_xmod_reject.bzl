// RUN: %bezelc -c -o %t.lib.o %S/pure_operator_rule6_3610_xmod_lib.bzl
// RUN: %not %bezelc -c %s --import %t.lib.o 2>&1 | FileCheck %s
//
// The cross-module twin of pure_operator_rule6_3610_local_binary_reject.bzl:
// `Vec::operator -` is declared in an IMPORTED module and is not
// `pure`-declared. §22.15's calls-to-pure-members rule reads
// `MemberSymbol::isPure` (fix #3609's both-sources channel), so the
// importer-side answer must match the exporter's own compilation — this is
// not a seam gap, per the #3610 issue body ("the imported half of it would
// otherwise be misfiled under the imported-symbol umbrella").
//
// CHECK-NOT: internal:
module App3610X;
import OpLib3610;

public class Program
  public static pure method P(a: Vec, b: Vec): int
    return a - b;
  end P;
end Program;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: call to `operator -` is not admitted in a pure body
// CHECK-SAME: (§22.15)
