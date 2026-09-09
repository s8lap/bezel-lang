// Ported from tests/sema/builtin_exceptions_test.cpp
//   (BEZEL_TEST sema_exc, exception_resolves).
//
// Narrowing fix #4025: a builtin EXCEPTION class type (one of
// `kBuiltinExceptionHierarchy`, §31) is heap-pointer class-typed, so a
// non-nullable field of that type without an initializer is now correctly
// REJECTED with the §7.9 diagnostic — the pre-#4025 rt->declaration ==
// null path silently skipped every builtin (incl. exceptions), pinning
// the bug. Test now pins the spec-correct REJECT side rather than the
// prior buggy ACCEPT.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public _e: Exception; end C;

// CHECK: non-nullable field `_e` needs an initializer or a constructor that assigns it (§7.9)
