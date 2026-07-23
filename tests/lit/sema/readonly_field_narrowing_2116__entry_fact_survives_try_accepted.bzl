// §7.4 (v0.214) / #2116: a fact that already holds at TRY ENTRY survives the
// whole construct — it is in the entry ∩ after-body catch-entry state (an
// ambient readonly-field fact is never invalidated), in each catch exit, and
// in the after-try join — so the catch body's use AND the after-try use are
// both ACCEPTED. No diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; public method Noise() end Noise; public method Len(): int if _name <> null then try Noise(); catch Exception => return _name.Length; end; return _name.Length; end; return 0; end Len; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
