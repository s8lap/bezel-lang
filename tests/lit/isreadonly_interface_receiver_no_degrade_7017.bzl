// fix #7017 (stdlib §4.1 / §4.2, language spec §27.6, v0.340): the
// `IsReadOnly` read through a BUILTIN COLLECTION INTERFACE receiver —
// `ICollection<T>` (stdlib §4.1), `IList<T>` (§4.1, which inherits it),
// `IOwningCollection<T>` (§4.2) and `IOwningList<T>` (§4.2, which inherits
// it) — must LOWER, not degrade.
//
// THE DEFECT. The member is DECLARED on those heads (stdlib §4.1:
// "readonly property IsReadOnly: bool;" on `ICollection<T>`; §4.2: the same
// on `IOwningCollection<T>`), so the #6997 sema arm — which rejects a bare
// reference to a name a head does NOT declare — admits the read, and nothing
// lowered it. `lowerLocalDecl`'s #2434 defense-in-depth zero-init fallback
// stored a zero-initialised value in the local's slot and emitted:
//
//     internal: lowerLocalDecl's initializer expression failed to lower to a
//     value for this local's slot (degraded to a zero-initialized value) — #2434
//
// WHY A LIT TEST AND NOT A gtest ROW. The runtime VALUE of the annotated form
// does NOT distinguish fixed from unfixed: stdlib §4.1 fixes the member's
// value to `false` ("`IsReadOnly` is `false` for every standard collection
// implementing `ICollection<T>` …"), and the degraded zero value of `bool` is
// also `false`, so the program printed `False` either way. What DOES
// distinguish them is the `internal: … — #2434` Warning — but `runAot`'s
// harness (tests/support/aot_support.hpp) discards compiler stderr on a
// successful compile, so no gtest row can see it. A lit test drives the real
// `bezelc` CLI and inspects raw stderr regardless of outcome, so it is the
// correct — and only — mechanical oracle for this half of the fix. Modeled
// directly on tests/lit/qualified_builtin_threading_module_qualified_no_degrade_5012_5013.bzl
// (the same "the value is identical either way, only the warning differs"
// situation) and on tests/lit/local_decl_zero_init_fallback_warns_2434.bzl.
//
// ACCEPT-WITH-NO-WARNING: bezelc -c must exit zero (the program is legal both
// before and after this fix) AND must NOT emit the #2434 internal warning
// (pre-fix it did; post-fix the read lowers to the member's value, so the
// fallback path is never reached).
//
// A correctly-fixed compile emits NO diagnostics at all for this program, so
// stdin to FileCheck is legitimately empty — a pure CHECK-NOT with nothing
// else matches vacuously true on empty input in general, but FileCheck
// itself errors on a truly empty stream unless told to allow it (measured:
// "FileCheck error: '<stdin>' is empty" — same fix already used by the
// #5012/#5013 case above and its siblings, `--check-prefix=NODIAG
// --allow-empty`).
//
// THE NEEDLE IS THE WHOLE SENTENCE (reviewer N6, this lane's fix-forward).
// The first cut pinned only the PREFIX "internal: lowerLocalDecl's
// initializer expression failed to lower", which is not vacuous today (the
// base compiler emits exactly that sentence, so the row is RED without the
// fix) but would become vacuous if the warning were ever REWORDED — a
// rewording that kept the prefix would still match, so the row would keep
// passing while the degradation it pins came back. Pinning the sentence in
// full — byte-for-byte the one tests/lit/
// local_decl_zero_init_fallback_warns_2434.bzl pins with a `CHECK:` — means
// a rewording now BREAKS this row instead of silently blinding it. Both
// arms are preserved: the base compiler emits this exact sentence, so the
// row is RED without the fix, and the fixed compiler emits no diagnostic at
// all, so the row is GREEN with it.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
class C
  public static method Main(): int
    var a: Array<int> := [1,2,3];
    var co: ICollection<int> := a;
    var r: bool := co.IsReadOnly;
    Console.WriteLine(r);
    return 0;
  end Main;
end C;

// NODIAG-NOT: internal: lowerLocalDecl's initializer expression failed to lower to a value for this local's slot (degraded to a zero-initialized value) — #2434
