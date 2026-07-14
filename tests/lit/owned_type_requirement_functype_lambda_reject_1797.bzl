// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §28.1 "Type requirement" (v0.193), issue #1797 — the function-type
// slot and lambda-parameter arms of the same rule. §28.1 lists "function-type
// parameter slots" (`(owned T) -> ()`), "function-type return slots"
// (`() -> owned T`), and "lambda parameters" (`(owned x: T) => ...`) among the
// positions where `owned` requires a class, interface, or bare-type-parameter
// type; a primitive (or any other non-owning type) is a compile error. Before
// the fix all three compiled clean.
//
// The function-type-slot diagnostics carry no precise source location (the
// parser populates no span on a function-type AST node), so they are matched
// by message; the lambda-parameter diagnostic is pinned to its expression's
// line. FileCheck matches these in source order (F1, then F2, then G).
module M;
class R
  // `owned` on a function-type PARAMETER slot at a primitive type.
  public method F1(cb: (owned int) -> ()): () return; end F1;
  // CHECK: `owned` is not admissible on a function-type parameter slot of type a primitive value type
  // CHECK-SAME: (§28.1)

  // `owned` on a function-type RETURN slot at a primitive type.
  public method F2(cb: () -> owned int): () return; end F2;
  // CHECK: `owned` is not admissible on a function-type return slot of type a primitive value type
  // CHECK-SAME: (§28.1)

  // `owned` on a LAMBDA parameter with an explicit primitive type.
  public method G(): ()
    var f := (owned x: int) => x;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: `owned` is not admissible on a lambda parameter of type a primitive value type
    // CHECK-SAME: (§28.1)
    return;
  end G;
end R;
