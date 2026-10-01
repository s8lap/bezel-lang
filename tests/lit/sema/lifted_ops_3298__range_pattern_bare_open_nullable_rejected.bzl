// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #3298 (§19.3 / §16.6, spec v0.286; v0.310, #5143 ruling): "The rejection
// is keyed on the **kind** of pattern, not on which bounds are written, so it
// reaches the open-ended forms and the fully open `..` with the closed ones
// — a rule that counted bounds would give a range pattern and another range
// pattern of the same extent opposite verdicts" (§19.3). §16.6 states the
// same keying.
//
// The bare `..` is therefore rejected over an UN-narrowed nullable scrutinee
// even though it performs no ordering test of its own; the reason §19.3
// gives for the unbounded form is uniformity (v0.273), and the verdict is
// the same.
//
// fix #5173 / §19.3 (v0.310): a §7.4-narrowed non-null read of a nullable
// scrutinee is ADMITTED. The pre-fix form `var n: int? := 5` narrows `n` at
// the assignment (valueProvesNonNull(5, int?) is true), so after #5173 it
// would now silently admit. Re-authored here to use a parameter `n: int?`
// (no `nullNarrowed_` entry) so the un-narrowed verdict is pinned.
//
// The diagnostic text is also re-pinned: the v0.286 ruling's "pattern
// position performs no narrowing" clause is REMOVED by #5173 because
// pattern position NOW performs §7.4 narrowing (the same reading §7.11
// already has for binary operands).

module M; class C public static method Main(n: int?): int var v := (case n of .. => 1, _ => 2); return v; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a range pattern requires a non-nullable scrutinee — ordering admits no absent participant ({{§19.3}}, {{§16.6}}, {{§7.11}}); separate the absent case first (`case x of null => …, var v: T when v > 70 => …, _ => … end`), or narrow the scrutinee with `??` ({{§7.7}}), `!` ({{§7.8}}), or an explicit `if x <> null then` ({{§7.4}}) before the `case`
