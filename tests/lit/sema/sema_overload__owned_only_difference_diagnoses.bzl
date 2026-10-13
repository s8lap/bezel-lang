// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// FLIPPED FROM ACCEPT TO REJECT for #5940 (§22.6 *`owned` and overload
// resolution*, §11.3, v0.338; proposal 0185).
//
// `owned` never distinguishes overloads. The ordinary-`owned` vs plain pair on
// a concrete class type therefore clashes at the second declaration: the
// declaration guard that admitted it (proposal 0091 §5, #4939) is retired.
// Pre-v0.250 shape restored.
//
// per §22.6 / §11.3 (v0.338, #5940)

module M; class T end T; class C public method F(t: T) _ := 1; end F; public method F(owned t: T) _ := 2; end F; end C;

// CHECK: overload of `F` is indistinguishable from another overload — overloads must differ in parameter count, types, or `ref`/`out` modifiers; `owned` and `scoped` never distinguish overloads
// CHECK-SAME: (§22.6/§11.3, v0.338)
