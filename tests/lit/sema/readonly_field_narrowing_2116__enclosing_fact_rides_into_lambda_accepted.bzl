// §7.4 (v0.214) / #2116: an AMBIENT-rooted readonly-field fact (`this._f` /
// static) established in the enclosing region is valid INSIDE a lambda body
// created in that region: a readonly field observed non-null holds that value
// forever after (§22.8 single assignment; a during-construction observation
// saw the final value, §22.7), so the fact is true whenever the closure later
// runs. The flagship §7.4 idiom — an injected optional dependency used from a
// callback without the copy-to-local dance. ACCEPT with no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; public method Use(): int if _name <> null then var f := lambda() => _name.Length; return f(); end; return 0; end Use; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
