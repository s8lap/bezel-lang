// #6725 (§8.3, v0.340, proposal 0196): the Object-slot `Equals` declares no
// default value. On a class in either spelling, an own-typed `Equals(other: C?)`
// with `override` fills the slot, so a default on its parameter is a compile
// error at the declaration, and the diagnostic names the remedy.
//
// REJECT: a class's own-typed `override Equals` with a default parameter value.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; public class C public override method Equals(other: C? := null): bool => true; public override method GetHashCode(): int => 0; end C; public class Program public static method Main(): int return 0; end Main; end Program;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member `Equals` fills the `Object` slot of class `C` and takes no default value: drop the default on its parameter (§8.3)
