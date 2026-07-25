// §7.4 / §22.16 / #2188: the companion to the reject cell — the fix is SCOPED
// to suppress only BODY flow facts at the clause boundary; it must NOT over-
// reject a `requires` clause whose member access is on a NON-nullable parameter
// (no narrowing needed — `s: string` is non-null by declared type). This is the
// legitimate accept shape, and it stays accepted. (The body's own post-guard
// `return s.Length` narrowing is likewise unaffected — the clear fires only
// AFTER the body visit, before the clause loop.)
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Len(s: string): int requires(s.Length >= 0) return s.Length; end Len; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
