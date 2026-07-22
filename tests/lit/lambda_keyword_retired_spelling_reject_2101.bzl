// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
// migrate_lambda_keyword: skip-file  (this file holds the RETIRED spelling on
// purpose — the migration script must never rewrite it)
//
// Spec §27.3 / A.7 / B.1, v0.217 (proposal 0064; impl #2101) — HARD CUTOVER:
// the pre-v0.217 lambda spellings `(params) => body` and `async (params) =>
// body` leave the grammar; each becomes a parse error whose diagnostic names
// the keyword-led replacement (the "insert `lambda`" fix-it). One retired
// lambda yields ONE diagnostic (the parser swallows the arrow and body), so
// the three sites below produce exactly three errors, in source order.
module M;
class C
  // Retired single-param head: parses as a parenthesized expression, trips
  // on the `=>` where `;` was expected.
  public static method A(): int
    var f := (x) => x * 2;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: `(params) => body` is the retired pre-v0.217 lambda spelling
    // CHECK-SAME: insert `lambda` before the parameter list
    // CHECK-SAME: (§27.3, v0.217)
    return 0;
  end A;

  // Retired TYPED-param head in call-argument position: fails inside the
  // parens at the `:` (the error-path scan names the migration).
  public static method B(): int
    var xs: Array<int> := [1, 2, 3];
    return xs.Where((x: int) => x > 1).Count();
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: `(params) => body` is the retired pre-v0.217 lambda spelling
    // CHECK-SAME: (§27.3, v0.217)
  end B;

  // Retired async form: recognized at `async (` and named directly.
  public static method D(): int
    var g: () -> Task<int> := async () => do return 1; end;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: `async (params) => body` is the retired pre-v0.217 lambda spelling
    // CHECK-SAME: insert `lambda` after `async`
    // CHECK-SAME: (§27.3, v0.217)
    return 0;
  end D;
end C;
