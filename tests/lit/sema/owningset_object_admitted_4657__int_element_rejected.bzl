// Control: OwningSet<int> must STILL REJECT — int is a primitive value type,
// not a class, and does not satisfy OwningSet<T>'s `where T: class` constraint.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public static method Main(): int using var s := new OwningSet<int>(); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `OwningSet` requires a class or interface element type (stdlib §4.2 / §14.4, #1327) — a primitive value type has no disposal obligation and cannot be owned in an owning collection. Use `Set<…>` for value elements.
