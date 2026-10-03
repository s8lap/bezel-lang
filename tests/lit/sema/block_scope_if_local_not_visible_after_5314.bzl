// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #5314 / spec §21.6 + §21.1 (v0.313): a local declared in an `if` body
// is visible only until the matching `end`. After the block the name is
// unresolved, so reading `inner` there is an error (and is NOT silently
// resolved to the leaked body local).
//
// REJECT: a read of an if-body local after the `if`.

module M; class C public static method Main(): int var s := 0; if s = 0 then var inner := 5; s := s + inner; end; return s + inner; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: unresolved identifier `inner`
