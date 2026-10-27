// RUN: %bezelc -c -o %t.lib.o %S/pure_indexer_rule6_3721_xmod_lib.bzl
// RUN: %not %bezelc -c %s --import %t.lib.o 2>&1 | FileCheck %s
//
// The cross-module twin of pure_indexer_rule6_3721_local_reject.bzl:
// `Bag`'s indexer `get` is declared in an IMPORTED module and is not
// `pure`-declared. The wire seam #3618 built for named-property accessor
// purity already carries an indexer's `MetaMember::accessorIsPure`
// identically (ModuleLoader.cpp's Indexer reconstruction arm mirrors its
// Property arm), so the importer-side answer matches the exporter's own
// compilation of the character-identical source.
//
// #6620: `internal:` is codegen-only and cannot occur on a row that
// rejects in the front end; this needle is the front end's own ICE marker.
// CHECK-NOT: internal compiler error
module App3721X;
import IdxLib3721;

public class Program
  public static pure method P(b: Bag): int
    return b[3];
  end P;
  public static method Main(): int => 0;
end Program;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: indexer read is not admitted in a pure body
// CHECK-SAME: (§22.15)
