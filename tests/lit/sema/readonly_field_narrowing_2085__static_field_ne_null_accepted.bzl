// §7.4 (v0.214, proposal 0058, #2085): a `static readonly` nullable field
// narrows off its type name (`TypeName._s`) after a `<> null` guard — the
// nullptr-rooted static path, never invalidated by a call/await/closure.
// ACCEPT with no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Counter public static readonly _label: string? := "hi"; public static method Main(): int if Counter._label <> null then return Counter._label.Length; end; return 0; end Main; end Counter;

// NODIAG-NOT: error
// NODIAG-NOT: §
