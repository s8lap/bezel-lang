// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// The DIAGNOSTIC-SILENCE oracle for fix #3098 (language spec §17.7
// "Standard-library builtins have a closed constructor surface", v0.241;
// §31.4; stdlib §3.3).
//
// §17.7 KEEPS the positional primary constructors of the two record-shaped
// builtins: "For a builtin record whose only declared constructor is its
// positional primary constructor (`ExceptionInfo`, §31.4; `ReadOnlyStringSpan`,
// stdlib §3.3), the UNDECLARED PARAMETERLESS form `new T()` is rejected".
//
// Measured on 663a68e, the kept spelling reached `lowerLocalDecl`'s zero-init
// fallback instead — `internal: lowerLocalDecl's initializer expression failed
// to lower to a value for this local's slot (degraded to a zero-initialized
// value) — #2434` — producing a record of null strings whose first
// concatenation exited 139 (SIGSEGV). The gtest sibling
// (tests/codegen/builtin_record_positional_ctor_3098_test.cpp) owns the values
// and the crash; this file owns the silence, which `runAot` cannot see (it
// records diagnostics only on a compile REJECT).
//
// ⚠ The §17.7 REJECTION of the parameterless form must not fire on the
// positional form — asserted here as well, so the two halves of the rule cannot
// drift into each other.
//
// ⚠ This file does NOT assert the absence of the §28.3 auto-drop note. Every
// value-shaped builtin `new` bound to a local emits it today — `new
// JsonOptions(…)`, which has had a `lowerNew` arm since #687, emits it too —
// so it is a pre-existing classification residual this fix neither causes nor
// closes, filed separately.
//
// NODIAG-NOT: internal:
// NODIAG-NOT: #2434
// NODIAG-NOT: is not a declared constructor of
// NODIAG-NOT: is not admitted at any argument list

module ACC3098;

// §17.7's own reference behaviour: "exactly as for an identically-shaped user
// record".
record UR(TypeName: string, Message: string, StackTrace: string);

class ACC
  public static method Main(): int
    var total: int := 0;

    // §31.4: `public record ExceptionInfo(TypeName: string, Message: string,
    // StackTrace: string);`
    var e := new ExceptionInfo("TypeName", "Msg", "Stack");
    Console.WriteLine("[" + e.TypeName + "]");      // the SIGSEGV arm
    total := total + e.TypeName.Length + e.Message.Length + e.StackTrace.Length;

    // §22.5's named-argument spelling, permuted, and the mixed form.
    var f := new ExceptionInfo(Message := "Msg", StackTrace := "Stack",
                               TypeName := "TypeName");
    var g := new ExceptionInfo("TypeName", StackTrace := "Stack",
                               Message := "Msg");
    if e.TypeName = f.TypeName and f.TypeName = g.TypeName then
      total := total + 1;
    end;

    // The identically-shaped user record §17.7 names as the reference.
    var u := new UR("TypeName", "Msg", "Stack");
    if u.Message = e.Message then total := total + 2; end;

    // stdlib §3.3: `public record ReadOnlyStringSpan(Source: string,
    // Start: int, Length: int): IReadOnlySpan<char>` — and stdlib §3.3's own
    // `Slice` is defined as `new ReadOnlyStringSpan(Source, this.Start + start,
    // length)`, i.e. the standard library constructs one this way.
    var s := new ReadOnlyStringSpan("abcdef", 1, 3);
    Console.WriteLine(s.ToString());
    total := total + s.Length + int(s[0]);

    // The already-working producer, as the agreement oracle (stdlib §9.6).
    var a := "abcdef".AsSpan(1, 3);
    if s.SequenceEqual(a) then total := total + 4; end;

    Console.WriteLine(total);
    return 0;
  end Main;
end ACC;
