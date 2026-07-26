// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §7.4 (v0.226, #2084): the narrowing-rejection diagnostic for a MUTABLY-
// CAPTURED local must name the branch-correct remedy — the simple `if x <>
// null` guard is insufficient because a closure invoked between the test and
// the use could null the local (#1566, v0.182). The new diagnostic tells the
// user to copy to an *uncaptured* local (the §7.4 spec's wording). Before
// #2084 this fired the same generic "on a nullable value requires narrowing"
// message as the non-captured case, which misled the user to add a no-op
// guard. The constructor call between the test and the use is what makes
// the narrowing invalid (§7.4 v0.182 — a control-transfer op invalidates a
// mutably-captured local's narrowing). Companion to
// nullable_narrowing_2084__field_rejected_remedy_names_copy_to_local.bzl.

module M; class Thing public constructor() end; end Thing; class C public static method Run(): int var s: string? := "hi"; var f := lambda() => do s := null; end; if s <> null then _ := new Thing(); return s.Length; end; return -1; end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot narrow `T?` accessed through a mutably-captured local
// CHECK-SAME: (§7.4)
