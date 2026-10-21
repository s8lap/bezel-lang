// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// The ANTI-OVER-REJECT control for #2994's §17.7 closed-constructor-surface
// rejection (v0.241, PR #3043).
//
// The rejecting population is name-keyed to exactly §17.7's two enumerated
// sets, so what must be proved unharmed is everything adjacent to them:
//
//   * every DECLARED PRODUCER of the rejected types — `Guid.Empty`,
//     `Guid.NewGuid()`, `Guid.Parse`, `default(Guid)` (stdlib §2.6);
//     `IntPtr.Zero`, `default(IntPtr)`, and the §5.2 explicit CONVERSION
//     `IntPtr(longValue)` — which is a CallExpr, not a `new`, and is exactly
//     why the rejection is keyed on the NewExpr node;
//   * the DECLARED positional primary constructors §17.7 keeps —
//     `new ExceptionInfo(…)` and `new ReadOnlyStringSpan(…)` — plus the real
//     `ReadOnlyStringSpan` producer `"…".AsSpan(start, length)` (stdlib §3.3);
//   * §14.2's generic body, which §17.7 says is "explicitly unaffected": a
//     `where T: new()` factory instantiated at `Guid` and at `DateTime`;
//   * a USER type sharing a builtin's name (§11.3), which carries a real
//     declaration and is excluded before the name is consulted;
//   * an identically-shaped USER record — the reference behaviour §17.7's
//     arm 2 is written as an equivalence to — and a body record, whose
//     parameterless form stays legal;
//   * the REST of `kNonHeapReservedBuiltins`, which the ruling deliberately
//     leaves alone: `new DateTime(2024, 1, 2)` is a member of the very table
//     the two arms are carved out of, and a fix keyed on the whole table
//     instead of on §17.7's two sets would have rejected it.
//
// ⚠ THIS FILE DELIBERATELY DOES NOT ASSERT `NODIAG-NOT: internal:`, unlike its
// #2758 sibling — but the reason has SHRUNK, and the note is updated rather
// than left to rot:
//   * `new ExceptionInfo(…)` and `new ReadOnlyStringSpan(…)` — §17.7 KEEPS
//     these declared positional primary constructors, and when this file was
//     written `lowerNew` had no arm for either, so both reached #2434's
//     zero-init fallback and produced a record of null strings. **fix #3098
//     grew those arms**, so these two no longer emit anything: their silence is
//     now asserted outright in
//     tests/lit/builtin_record_positional_ctor_accept_3098.bzl.
//   * the `where T: new()` body's `new T()` — #2477's dead-abstract-template
//     note, which every generic factory in the tree emits. THIS is what still
//     keeps a blanket `NODIAG-NOT: internal:` off this file.
// The oracle here is exactly that no §17.7 diagnostic fires and the file
// compiles.
//
// NODIAG-NOT: §17.7
// NODIAG-NOT: is not admitted at any argument list
// NODIAG-NOT: is not a declared constructor of

module ACC2994;

record UR(A: string, B: string, C: string);

record BR
  public N: int := 3;
  public constructor() end constructor;
end BR;

class MyGuid
  public readonly property N: int := 7;
end MyGuid;

class Factory
  // §14.2: "`new T()` for a value substitution is equivalent to `default(T)`".
  public static method Count<T>(): int where T: new()
    var t := new T();
    return 1;
  end Count;
end Factory;

class ACC
  public static method Main(): int
    // The declared `Guid` producers §17.7's diagnostic names.
    var a := Guid.Empty;
    var b := default(Guid);
    var c := Guid.NewGuid();
    var d := Guid.Parse("00000000-0000-0000-0000-000000000000");
    Console.WriteLine(a.ToString());
    var total := a.ToString().Length + b.ToString().Length +
                 c.ToString().Length + d.ToString().Length;

    // The declared `IntPtr` producers, plus the §5.2 explicit conversion.
    var p := IntPtr.Zero;
    var q := default(IntPtr);
    var s := IntPtr(7L);
    if p = q then total := total + int(long(s)); end;

    // The DECLARED positional primary constructors §17.7 keeps.
    var ei := new ExceptionInfo("T", "M", "S");
    var rs := new ReadOnlyStringSpan("abcdef", 1, 3);

    // The real `ReadOnlyStringSpan` producer (stdlib §3.3).
    var span := "abcdef".AsSpan(1, 3);
    Console.WriteLine(span.ToString());
    total := total + span.Length;

    // §14.2's generic body at a `Guid` and a `DateTime` substitution.
    total := total + Factory.Count<Guid>() + Factory.Count<DateTime>();

    // A USER type sharing a builtin's name.
    using var mine := new MyGuid();
    total := total + mine.N;

    // The identically-shaped user record, and a body record's parameterless
    // form (declared no-arg ctor; a body record without one would not have
    // a `new BR()` admit, per §9.1 / §9.2 / #4059 — see the sibling lit
    // test `body_only_record_zero_arg_new_4059` for the rejection shape).
    var ur := new UR("a", "bb", "ccc");
    var br := new BR();
    total := total + ur.A.Length + br.N;

    // The rest of kNonHeapReservedBuiltins, deliberately untouched.
    var dt := new DateTime(2024, 1, 2);
    using var buf := new OwningBuffer<int>(4);
    var rg := 0..5;
    total := total + dt.Day + rg.End!;

    Console.WriteLine(total);
    return 0;
  end Main;
end ACC;
