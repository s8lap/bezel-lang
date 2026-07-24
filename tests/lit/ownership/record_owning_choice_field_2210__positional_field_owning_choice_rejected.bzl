// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / §35.4.1 (v0.226, issue #2210): the issue's exact repro. A record
// field whose type is an owning-classified choice (the choice's case has an
// `owned` class payload) is rejected at the field DECLARATION — records
// declare no disposal path (§9.7: no destructor, no `owned` field, no
// `IDisposable`), and §35.4 makes owning choices move-only value types
// whose active case carries payloads requiring discharge at scope end
// (§28.3). The structural classification is consulted at the declaration
// boundary so the error names the field and the rule, not the construction
// site (which would diagnose a burial leak rather than a missing disposition).

module M; class Res public method V(): int return 5; end V; end Res; choice Own Some(owned r: Res), None; end Own; record HolderO(K: Own); class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `HolderO` may not have an owning-classified choice field `K`
// CHECK-SAME: (§35.4, §9.7)
