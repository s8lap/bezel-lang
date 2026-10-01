// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #3298 (§19.3 / §16.6 / §7.11, spec v0.286; v0.310, #5143 ruling): "A
// **nullable** scrutinee — value or reference — is a compile error at a
// relational pattern, and at a range pattern (§16.6) alike: ordering admits
// no absent participant (§7.11)" (§19.3).
//
// The REFERENCE half is what this file pins. The first draft of the #3298
// gate keyed on `Primitive && nullable`, so a nullable CLASS-typed scrutinee
// slipped past it and — with the backend's nv.relop.match lowering deleted —
// reached codegen and emitted `icmp sgt ptr, i32`, a malformed-IR compiler
// abort where §19.3 requires a diagnosis.
//
// fix #5173 / §19.3 (v0.310): a §7.4-narrowed non-null read of a nullable
// scrutinee is ADMITTED. The pre-fix form `var k: K? := null` is still
// un-narrowed (a `null` literal's `valueProvesNonNull` is false), so this
// rejection is preserved. The diagnostic's "pattern position performs no
// narrowing" clause is REMOVED by #5173 because pattern position NOW
// performs §7.4 narrowing.

module M; class K public V: int := 0; end K; class C public static method Main(): int var k: K? := null; var v := (case k of > 3 => 1, _ => 2); return v; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a relational pattern requires a non-nullable scrutinee — ordering admits no absent participant ({{§19.3}}, {{§16.6}}, {{§7.11}}); separate the absent case first (`case x of null => …, var v: T when v > 70 => …, _ => … end`), or narrow the scrutinee with `??` ({{§7.7}}), `!` ({{§7.8}}), or an explicit `if x <> null then` ({{§7.4}}) before the `case`
