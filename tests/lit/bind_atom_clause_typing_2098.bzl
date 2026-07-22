// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §17.15 / §21.1 / §7.1 / §22.16, v0.216 (impl #2098) — "each clause
// types exactly as the §21.1 declaration statement it is spelled as": the
// §7.1 bare-`null` reject applies verbatim inside the atom, and a contract
// clause rejects the marked-transfer clause spelling (`var y := owned x;`),
// preserving §22.16's borrow-only guarantee.
module M;
class C
  // (1) §7.1: a bare `null` clause initializer requires a nullable
  // ascription — the same diagnostic as the statement form.
  public static method A(): int
    return (var x := null; 0);
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: cannot infer a type for `x` from a bare `null`
    // CHECK-SAME: (§7.1)
  end A;

  // (2) §22.16 / §17.15: an atom clause spelled as a marked transfer is
  // rejected in a contract clause.
  public method B(n: int): int
    requires((var m := owned n; m > 0))
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: marked binding transfer
    // CHECK-SAME: borrow-only guarantee (§22.16 / §17.15, v0.216)
    return n;
  end B;
end C;
