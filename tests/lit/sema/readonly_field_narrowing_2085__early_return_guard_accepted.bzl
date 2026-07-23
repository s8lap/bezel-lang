// §7.4 (v0.214, proposal 0058, #2085): the early-exit form narrows a readonly
// field too — after `if _name = null then return; end;` the fall-through path is
// narrowed non-null (the `= null` branch always exits), so the later `_name`
// use is admitted. ACCEPT with no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; public method Len(): int if _name = null then return 0; end; return _name.Length; end Len; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
