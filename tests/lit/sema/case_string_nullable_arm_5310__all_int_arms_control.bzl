// Control row for #5310 — must ACCEPT both pre-fix and post-fix.
//
// All-`int` arms, the shape the #1952 comment names as DELIBERATELY left on the
// pre-#1952 fold ("a VALUE-type null-arm result (`int?`, `bool?`, record?, enum?)
// keeps the pre-#1952 fold UNCHANGED … re-typing it via the general
// nullable-form rule regressed the value-nullable result", #573). This row has
// no null arm, so it pins that the widened string rule did not leak into the
// value-type arms.
//
// Related: §19.2 (v0.313) case-expression typing, §28.3 (v0.313) arm-meet.

// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var i := 1; var b := (case i mod 2 of 0 => 1, _ => 2); return b + 1; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: warning
// NODIAG-NOT: §
