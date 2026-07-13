// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/generic_default_new_constraint_864_test.cpp
//   (BEZEL_TEST generic_default_new_constraint_864, part2_requires_arg_violates_new).
//
// §14.2 (#864 part 2): calling `Make<RequiresArg>()` where RequiresArg
// has only a parameterised constructor violates `where T: new()` → error.

module M; class RequiresArg public _n: int := 0; public constructor(n: int) _n := n; end constructor; destructor end destructor; end RequiresArg; static class Utils public static method Make<T>(): int where T: class, new() using var t := new T(); return 1; end Make; end Utils; class C public static method Main(): int return Utils.Make<RequiresArg>(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `RequiresArg` does not satisfy the constraint `where T: new()`
// CHECK-SAME: (§14.2)
