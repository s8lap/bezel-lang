// narrow issue 3440 (stdlib §5 *Filtering*; spec §8.1, §12, §12.7; v0.283)
// — the COLLECTION-receiver sema arm.
//
// `OfType<U>()` emits no runtime type test at all: codegen builds a
// `bz_lazy_where` whose predicate trampoline is a constant `ret i32 1`, and
// sema types the result `IEnumerable<U>`, so every element is relabelled `U`
// untested. The program below is `OfType`'s canonical, entirely legal use — an
// `OwningArray<Animal>` holding one `Cat`, asked for its `Dog`s — and before
// this interim it compiled clean, reported ONE element, and read a `Dog`-only
// field 8 bytes past the end of the `Cat` (a heap-buffer-overflow under ASan,
// a SIGSEGV without one, silent and exit-0 on a plain build).
//
// This file pins the WORDING, not merely the rejection, because the wording is
// the whole of the claim being made. No normative document states ANY semantics
// for `OfType<U>()` — `Bezel_Standard_Library.md` carries its bare signature and
// its name in one list, and `Bezel_Language_Spec.md` and `Bezel_Changelog.md` do
// not mention it (issue 3441) — so as far as anything ever specified, this
// program is LEGAL. The diagnostic must therefore say the compiler has not
// implemented the case, and must not say the program is wrong. It is also an
// interim: `OfType<U>` is removal candidate R2 of proposal 0121 (issue 3409,
// owner ruling pending), and this rejection commits to nothing about what the
// operator means.
//
// The sibling arm (a lazy receiver, reached through `Where`/`Select`/a `Range`)
// is pinned by `oftype_unsound_relabel_lazy_arm_3440.bzl`; both arms must reject
// or the fix is a half-fix that reads as a whole one. The accept-controls — the
// sound cells still compiling AND still producing their values — live in
// `tests/sema/oftype_unsound_relabel_3440_test.cpp`.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M3440Coll;

class Animal
  public Tag: int := 1;
end Animal;

class Cat: Animal
end Cat;

class Dog: Animal
  public DogOnly: int := 77;
end Dog;

class C
  public static method Main(): int
    using var xs := new OwningArray<Animal>();
    xs.Add(new Cat());
    var n := 0;
    for var d in xs.OfType<Dog>() do n := n + d.DogOnly; end;
    return n;
  end Main;
end C;

// (1) The diagnostic is LOCATED at the call. The line and column patterns
//     exclude 0 explicitly: a diagnostic reported at 0:0 points the reader at
//     nothing, and this rule's whole value is telling them which call it is.
// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: `OfType<Dog>()` on a sequence of `Animal`
// (2) NOT-IMPLEMENTED framing. The program is legal as far as anything ever
//     specified; what is missing is the compiler's type test.
// CHECK-SAME: is not yet implemented
// (3) The declaring section and the tracking issue, so the reader can find both
//     the surface and the reason it is unimplemented.
// CHECK-SAME: (stdlib §5 / issue 3440)
// (4) The mechanism, stated concretely rather than as a category: what is
//     missing, and what the compiler would otherwise do instead.
// CHECK-SAME: needs a per-element runtime type test and the compiler emits none
// CHECK-SAME: relabel every `Animal` as `Dog` untested
// (5) The boundary — which cases ARE implemented — with its spec authorities,
//     so the message answers "then what can I write?" without a round trip.
// CHECK-SAME: spec §12
// CHECK-SAME: spec §8.1
// (6) A remedy that exists: `Where` plus the §12.7 `is` test.
// CHECK-SAME: Filter explicitly with `Where` and an `is` test (spec §12.7)
// (7) It must NOT blame the program. "is not a" / "cannot be" phrasings would
//     assert a rule no document states.
// CHECK-NOT: is not a valid
// CHECK-NOT: must be a subtype
