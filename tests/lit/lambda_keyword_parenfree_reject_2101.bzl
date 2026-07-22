// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §27.3, v0.217 (proposal 0064 §6; impl #2101): the shorthand tier is
// deliberately EXCLUDED — no paren-free single-parameter form (`lambda x =>
// e` collides with the arm form `x => e`) and no paren-free zero-parameter
// form (`lambda => e`); zero parameters spell the mandatory empty pair
// (`lambda() => e`). Both shapes are parse errors naming the rule.
module M;
class C
  public static method A(): int
    var f := lambda x => x * 2;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: expected '(' after `lambda`
    // CHECK-SAME: there is no paren-free form
    // CHECK-SAME: (§27.3, v0.217)
    return 0;
  end A;

  public static method B(): int
    var g := lambda => 42;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: expected '(' after `lambda`
    // CHECK-SAME: zero parameters spell the empty pair
    // CHECK-SAME: (§27.3, v0.217)
    return 0;
  end B;
end C;
