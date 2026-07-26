// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §7.4 (v0.226, #2084): the narrowing-rejection diagnostic for a FIELD read
// must name the branch-correct remedy — "copy to a local first". The previous
// message ("on nullable field `_name` requires a local copy first") was
// correct but generic; the v0.226 form spells the field-§7.4 hazard ("may be
// reassigned between reads") and the field-copy idiom explicitly. Companion
// to nullable_narrowing_2084__mutably_captured_local_rejected_remedy_names_
// copy_to_uncaptured_local.bzl.

module M; class C public _name: string?; public constructor(n: string) _name := n; end constructor; destructor end destructor; public method Len(): int if _name <> null then return _name.Length; end; return 0; end Len; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot narrow `T?` accessed through a field
// CHECK-SAME: (§7.4)
