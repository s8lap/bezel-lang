// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3641 (§12.5 / §23.4, v0.286): the REJECT mirror of
// sema_abs_impl__abstract_class_indexer_satisfied_two_levels_up_ok.bzl —
// `A` declares the abstract indexer; the intermediate `B` is itself
// `abstract` and legitimately does not override it ("An abstract class
// need not override every abstract member of its base", §12.5); `C: B` is
// CONCRETE and never overrides it either. The candidate search on `C`'s
// own base-CLASS chain must walk PAST `B` (which contributes no candidate
// of its own) all the way to `A`'s abstract declaration, find nothing
// wireable, and report — not stop early at the first non-abstract
// ancestor, and not be satisfied merely because `B` was permitted to skip
// the obligation itself.

module M; abstract class A public abstract property [i: int]: int; end A; abstract class B: A end B; class C: B end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: concrete class `C` does not implement inherited abstract member `indexer`
// CHECK-SAME: (§12.5)
