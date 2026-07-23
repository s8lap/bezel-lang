// §7.4 (v0.214) / #2116: an ambient-rooted readonly-field fact survives an
// `await` — §7.4 (v0.214): "no call, `await`, or closure can invalidate a fact
// about its value". Another continuation interleaving at the suspension point
// (§30.13) has no writer for a readonly field, so the post-await `_name.Length`
// is ACCEPTED with no re-test. No diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; public async method Step(): Task<int> return 1; end Step; public async method Len(): Task<int> if _name <> null then var k := await Step(); return _name.Length + k; end; return 0; end Len; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
