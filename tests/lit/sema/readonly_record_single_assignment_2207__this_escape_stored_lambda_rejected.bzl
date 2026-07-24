// §22.8 / §22.7 / §7.4 (v0.226) — the #2207 probe-confirmed SEGV, now rejected.
//
// §22.8's single-assignment rule (§7.4/#2116 narrowing depends on it) is
// type-agnostic and applies to records. The exploit: a record's authored
// ctor double-assigns a readonly field across a this-escape helper that
// stashes a narrowing lambda; the invoked lambda derefs the field the ctor
// later nulled. The fix is the readonly once-check on records
// (runRecordConstructor in DefiniteAssignment.cpp) — rejects the second
// assignment.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; record R public readonly _f: string?; public _stash: (() -> int)?; public constructor() _f := "abc"; Helper(); _f := null; end constructor; public method Helper() if _f <> null then _stash := lambda(): int => _f.Length; end; end Helper; end R; class Prog public static method Main(): int using var r := new R(); var s := r._stash; if s <> null then return s(); end; return -1; end Main; end Prog;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to readonly field `_f` more than once
// CHECK-SAME: (§22.8 single-assignment)