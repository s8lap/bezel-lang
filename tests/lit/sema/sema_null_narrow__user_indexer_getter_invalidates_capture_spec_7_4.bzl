// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, user_indexer_getter_invalidates_capture_spec_7_4).
//
// §7.4 (#1593): a user-defined indexer access `g[i]` invokes the indexer getter
// (also an "accessor" per §7.4) — it invalidates a mutably-captured local too.

module M; class Grid public property [i: int]: int get => i; end; end Grid; class C public static method Run(g: Grid): int var s: string? := "hi"; var f := lambda() => do s := null; end; if s <> null then _ := g[3]; return s.Length; end; return -1; end Run; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot narrow `T?` accessed through a mutably-captured local
// CHECK-SAME: (§7.4)
