// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2153 (§6.6 / §7.4 / §4.6 / §7.11, spec v0.220): `string? += "x"` — §7.11's
// lifting is confined to nullable VALUE types and "does not extend" to
// `string` (the §4.6 ordering bullet states the boundary; the same
// use-requires-narrowing discipline of §7.4 governs every other operation on
// a nullable reference). Before the fix the nullable-LHS gate skipped the
// check and the #857 concat path dereferenced the null string pointer —
// `string?(null) += "x"` SEGFAULTED at runtime (probe, main @dd33abd).

module M; class C public static method Main(): int var s: string? := null; s += "x"; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment on a `string?` target requires a non-nullable `string` — §7.11's lifting covers nullable value types only and does not extend to `string`; narrow the target first (§7.4)
