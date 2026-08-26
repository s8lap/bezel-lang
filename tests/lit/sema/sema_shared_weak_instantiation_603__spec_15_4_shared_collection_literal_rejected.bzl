// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/shared_weak_instantiation_603_test.cpp
//   (BEZEL_TEST sema_shared_weak_instantiation_603, spec_15_4_shared_collection_literal_rejected).
//
// §15.4 / §28.13 (must-fail): collection literal passed to new Shared<T> —
// the constructor takes an owned T, not a collection literal.

module M; class Inner end Inner; class C public static method M() using var h := new Shared<Inner>([new Inner()]); end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: collection literal `[..]` cannot be passed as an argument to `new Shared<...>` — the constructor takes a single `owned` value of `T`, not a collection literal; bind the literal to a named typed binding first (`var xs: Shared<T> := [...]`) and transfer its elements through `Add`/`AddLast`/etc. (§15.4, #2725)
