// fix #3044 (§5.4 / §10.2, v0.242) OVER-REJECTION GUARD: "Under duplicate
// constant values (§10.2), the declared-value set is the set of distinct
// values: `E(1)` where `Legacy := 1, Current := 1` yields that one value."
// The spec's own worked example — verbatim — must compile.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum E Legacy := 1, Current := 1; end E; class C public static method Main(): int var e: E := E(1); return int(e); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
