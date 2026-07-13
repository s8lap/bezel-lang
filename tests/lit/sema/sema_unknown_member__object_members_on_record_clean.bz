// Ported from tests/sema/unknown_member_test.cpp
//   (BEZEL_TEST sema_unknown_member, object_members_on_record_clean).
//
// §8.1 / §8.5: the universal Object members are valid on a positional record
// (synthesized value-semantics members) — must not reject.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Point(x: int, y: int); class C public method M(p: Point): int var s := p.ToString(); var h := p.GetHashCode(); return h; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
