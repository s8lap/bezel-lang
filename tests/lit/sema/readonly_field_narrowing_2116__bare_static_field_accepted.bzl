// §7.4 (v0.214) / #2116: a `static readonly` nullable field also narrows
// through its BARE spelling inside the declaring type (`_label`, no
// `TypeName.` qualifier) — the same ambient-rooted fact as the qualified
// form, keyed by the (unique) FieldSymbol. ACCEPT with no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Counter public static readonly _label: string? := "hi"; public static method Main(): int if _label <> null then return _label.Length; end; return 0; end Main; end Counter;

// NODIAG-NOT: error
// NODIAG-NOT: §
