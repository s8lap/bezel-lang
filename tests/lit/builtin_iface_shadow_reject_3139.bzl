// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the compile is expected to fail —
// see tests/lit/README.md for why every reject test wraps `bezelc` in `not`.)
//
// Issue #3139 / spec §2.3 (Always-available names) / §18.5 (Interpolated
// strings), v0.254 — a LOCAL behaviour change that #3139 makes deliberately,
// pinned here because it IS a change and because it is spec-correct.
//
// WHAT CHANGED. §18.5's format-spec admission used to be decided by walking
// the operand's `ast::TypeDecl` base list for a base whose simple NAME was
// "IFormattable". #3139 replaced that name match with an IDENTITY test against
// the auto-imported interface. A user type may shadow that name — §2.3 is
// explicit: "The other always-available names — the auto-imported library
// types such as `Object`, `Array<T>`, `Task`, `Subscription`, and the
// iteration interfaces — may be shadowed by local declarations under the
// normal name-resolution rules" — and §18.5 is equally explicit that the
// opt-in is the auto-imported one: "a **user** type opts in by implementing
// the auto-imported interface `IFormattable`".
//
// So a class implementing a SHADOW is not IFormattable, and a format spec on
// it is a compile error naming §18.5.
//
// MEASURED, single-module builds, before (#3139's base) → after:
//
//   a shadow declaring the right member
//   (`interface IFormattable { ToString(format: string): string; }`,
//    `class Fake: IFormattable`)                  compiled, returned 77 → REJECT
//
//   a shadow declaring ONLY `Nope(): int` —
//   nothing for §18.5's dispatch to call at all   compiled, returned 11 → REJECT
//
// The second row is the one that makes the old behaviour indefensible rather
// than merely different: the name match never looked at the shadow's MEMBERS
// either, so a format spec was admitted on a type with no `ToString(format)`
// anywhere, and the hole then formatted via something else entirely. That is a
// plain over-accept, and it is what the identity test removes.
//
// THE MECHANISM IS THE SECOND ONE, not the flag. `declaresBuiltinInterface`
// tells the synthesized-symbol built-ins (IComparable, IEquatable, …) apart
// from a same-named user type via `TypeSymbol::isSynthesizedBuiltinInterface`.
// IFormattable is in the OTHER family — it has no TypeSymbol at all — and the
// shadow is excluded by a different route: a user-declared base HAS a
// declaration, so Type Resolution puts it in `baseTypes`/`baseTypeRefs` and it
// never lands in `nonSymbolBaseRefs`, which is the only place the non-symbol
// built-ins live. Two mechanisms, one rule: identity, not name.
//
// This file pins the `Nope()` row (the over-accept). The other row, and the
// §2.3 accept-control that the shadow stays USABLE, are pinned with their
// VALUES in the two `local_shadowed_iformattable_*` arms of
// tests/module/xmod_builtin_iface_conformance_3139_test.cpp.
module M3139Shadow;

// §2.3: shadowing an auto-imported library name is permitted. This shadow has
// no formatting surface whatsoever.
interface IFormattable
  method Nope(): int;
end IFormattable;

class Empty: IFormattable
  public method Nope(): int => 1;
end Empty;

class C
  public static method Main(): int
    using var e := new Empty();
    return (if $"{e:X}" = "" then 55 else 11);
  end Main;
end C;

// bezelc emits diagnostics as `file:line:col: message` with no "error:"
// severity word, so anchor on the `:line:col:` shape plus the message text.
//
// The §18.5 message is REUSED verbatim from the pre-existing plain-class
// reject, deliberately: to a user, a class implementing a shadow is a class
// that does not implement IFormattable, and there is nothing special to say.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a format spec requires an intrinsically-formattable operand
// CHECK-SAME: or one implementing `IFormattable`; `Empty` is neither
// CHECK-SAME: 18.5
