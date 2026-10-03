// Control row for #5310 — must ACCEPT both pre-fix and post-fix.
//
// All-string arms and NO null arm at all, so the §20 (v0.313) null-arm rule
// never fires: the result is plain `string`. This row pins the other boundary
// of the #1952/#5310 rule — the fix widened the rule from a non-nullable
// `string` arm to a nullable `string?` arm, and must not disturb a `case` whose
// arms were already non-nullable.
//
// Related: §19.2 (v0.313) case-expression typing, §28.3 (v0.313) arm-meet.

// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var i := 1; var b := (case i mod 2 of 0 => "a", _ => "b"); return (b + "x").Length; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: warning
// NODIAG-NOT: §
