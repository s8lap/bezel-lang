// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Spec §35.10 (v0.200, #2044): "Choices never satisfy `where T: new()`'s
// value-type arm (`default(C)` is undefined for every choice type, §17.11,
// §35.10)." This test exercises the constraint directly: a generic `Make`
// requiring `new()` instantiated with a choice type is rejected.

module M;

choice Opt Some(value: int), None; end Opt;

static class Utils public static method Make<T>(): int where T: new() using var t := new T(); return 1; end Make; end Utils;

class C public static method Main(): int return Utils.Make<Opt>(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Opt` does not satisfy the constraint `where T: new()`
// CHECK-SAME: (§14.2)
