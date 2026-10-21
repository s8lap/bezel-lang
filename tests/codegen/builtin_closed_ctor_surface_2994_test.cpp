// Spec-derived regression test for issue #2994 — STANDARD-LIBRARY BUILTINS
// HAVE A CLOSED CONSTRUCTOR SURFACE (language spec §17.7, v0.241; owner ruling
// interpretation (2), two-arm form, PR #3043).
//
// §17.7, verbatim (v0.241):
//
//   "**Standard-library builtins have a closed constructor surface.** For a
//    standard-library builtin type — any type the standard-library document
//    declares — the constructor surface is exactly what that document declares
//    — `new T(...)` resolves only against a declared constructor, never against
//    a compiler-supplied fallback. A builtin declaring **no** constructor —
//    `Guid` (stdlib §2.6), `IntPtr`/`UIntPtr` (stdlib §10.0) — admits no
//    `new T(...)` at any argument list; the diagnostic names the declared
//    producers — for `Guid`: `Guid.Empty`, `Guid.NewGuid()`, or `default(Guid)`
//    (§17.11); for `IntPtr`: `IntPtr.Zero` or `default(IntPtr)`. For a builtin
//    record whose only declared constructor is its positional primary
//    constructor (`ExceptionInfo`, §31.4; `ReadOnlyStringSpan`, stdlib §3.3),
//    the undeclared parameterless form `new T()` is rejected the same way —
//    exactly as for an identically-shaped user record. §14.2's generic-body
//    equivalence (`new T()` ≡ `default(T)` for a value substitution under
//    `where T: new()`) is unaffected: `Guid` satisfies `new()` — its `default`
//    is well-defined — so generic factories still mint empty Guids."
//
// stdlib §2.6 carries the matching insurance note: "`Guid` declares no
// constructor, so `new Guid(...)` is a compile error at any argument list
// (language spec §17.7's closed-constructor-surface rule); construct via
// `Empty`, `NewGuid()`, `FromByteArray`, `Parse`/`TryParse`/`ParseOrNull`, or
// `default(Guid)`."
//
// ── WHAT IT REPLACES ───────────────────────────────────────────────────────
//
// MEASURED ON 309356e (the parent), all of these compiled and reached their
// value THROUGH THE #2434 DEGRADE, not through a construction — `lowerNew` has
// no arm for them, returns nullptr, and `lowerLocalDecl`'s fallback stores
// `getNullValue(slotTy)` behind an `internal: … (degraded to a zero-initialized
// value) — #2434` message:
//
//   new Guid()  new Guid(1)  new Guid("x")      → the all-zero Guid, i.e. the
//                                                 right answer BY COINCIDENCE
//                                                 of representation
//   new IntPtr()  new IntPtr(0)  new UIntPtr()  → zero
//   var g: Guid := new();                       → same, via the §17.7 shorthand
//   private _g: Guid := new();                  → `internal: lowerExpr returned
//                                                 null for instance field/
//                                                 property initializer — #1386`
//   new ExceptionInfo()  new ReadOnlyStringSpan()
//                                               → a record of NULL strings
//                                                 (both types' fields are
//                                                 `string`) — the #2922 class,
//                                                 arriving through a silent
//                                                 fallback
//
// ── THE OTHER HALF OF THE #2922 HAZARD — CLOSED SEPARATELY BY #3098 ────────
//
// The ruling states that rejecting the parameterless form "still makes the
// #2434 degrade's null-`string` values unspellable, closing the #2922-class
// hazard at the root". Measured on the compiler this file was written against,
// that was half true: the DECLARED positional constructor degraded identically
// —
//
//   var e := new ExceptionInfo("T", "M", "S");
//   Console.WriteLine(e.TypeName);        printed an EMPTY LINE, exit 0
//
// — because `lowerNew` had no arm for it either. §17.7 says that form is
// declared and must keep working, so #2994 left it accepted and pinned the gap
// below as a KNOWN-GAP control. **fix #3098 grew those arms**, so the pin has
// FLIPPED: the assertion below now requires the supplied value, and the full
// behavioural grid lives in
// tests/codegen/builtin_record_positional_ctor_3098_test.cpp.
//
// ── ANTI-OVER-REJECT ───────────────────────────────────────────────────────
//
// The population is name-keyed to exactly §17.7's two enumerated sets, so the
// controls that matter are (a) every declared PRODUCER of the rejected types,
// (b) §14.2's generic body, (c) a USER type sharing a builtin's name, (d) an
// identically-shaped user record's parameterless-vs-positional behaviour, and
// (e) the rest of `kNonHeapReservedBuiltins`, which the ruling deliberately
// leaves alone (`new DateTime(2024, 1, 2)` in particular is a member of that
// registry table and must keep constructing).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string newLocal(const char* ctor) {
    return std::string(
        "module M; "
        "class C public static method Main(): int "
        "  var x := ") + ctor + "; "
        "  Console.WriteLine(\"constructed\"); "
        "  return 0; "
        "end Main; end C;";
}

bool hasError(const std::string& program, const char* needle) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The §17.7 discriminator. NOT `hasErrors()`: `runAot` reports a runtime
// SIGSEGV as an Error too, and pre-fix several of these programs emitted loud
// `internal:` residuals — so only the rule's own text distinguishes.
bool rejectsNoCtorArm(const std::string& program) {
    return hasError(program, "is not admitted at any argument list") &&
           hasError(program, "\xC2\xA7""17.7");
}
bool rejectsPositionalOnlyArm(const std::string& program) {
    return hasError(program, "is not a declared constructor of") &&
           hasError(program, "\xC2\xA7""17.7");
}

}  // namespace

// ── ARM 1: the no-constructor builtins, at ANY argument list ────────────────

BEZEL_TEST(builtin_closed_ctor_surface_2994,
           new_guid_is_rejected_at_every_argument_list_spec_17_7) {
    for (const char* ctor : {"new Guid()", "new Guid(1)", "new Guid(\"x\")"})
        BEZEL_EXPECT_TRUE(rejectsNoCtorArm(newLocal(ctor)));
}

BEZEL_TEST(builtin_closed_ctor_surface_2994,
           the_guid_diagnostic_names_the_declared_producers_spec_17_7) {
    // §17.7 names them explicitly, and stdlib §2.6 repeats the list. A
    // diagnostic that said only "no constructor" would leave the reader to find
    // the spelling that does exist — the #2935 re-wording lesson.
    for (const char* needle : {"Guid.Empty", "Guid.NewGuid()", "default(Guid)"})
        BEZEL_EXPECT_TRUE(hasError(newLocal("new Guid()"), needle));
}

BEZEL_TEST(builtin_closed_ctor_surface_2994,
           new_intptr_and_uintptr_are_rejected_spec_17_7_stdlib_10_0) {
    for (const char* ctor : {"new IntPtr()", "new IntPtr(0)", "new UIntPtr()"})
        BEZEL_EXPECT_TRUE(rejectsNoCtorArm(newLocal(ctor)));
    // §17.7 names `IntPtr.Zero` / `default(IntPtr)` for this one.
    BEZEL_EXPECT_TRUE(hasError(newLocal("new IntPtr()"), "IntPtr.Zero"));
    BEZEL_EXPECT_TRUE(hasError(newLocal("new IntPtr()"), "default(IntPtr)"));
}

BEZEL_TEST(builtin_closed_ctor_surface_2994,
           the_new_shorthand_is_the_same_construction_spec_17_7) {
    // §17.7's `new()` shorthand "may be used when the type is unambiguous" in a
    // typed local declaration and a field initializer — the same construction
    // with the target type taken from context, so the same rule applies.
    // Pre-fix the local form degraded through #2434 and the field form through
    // #1386's `internal: lowerExpr returned null …` residual.
    BEZEL_EXPECT_TRUE(rejectsNoCtorArm(
        "module M; class C public static method Main(): int "
        "  var g: Guid := new(); "
        "  Console.WriteLine(g.ToString()); return 0; end Main; end C;"));
    BEZEL_EXPECT_TRUE(rejectsNoCtorArm(
        "module M; "
        "class H private _g: Guid := new(); end H; "
        "class C public static method Main(): int "
        "  using var h := new H(); return 0; end Main; end C;"));
}

// ── ARM 2: the record-shaped builtins, PARAMETERLESS form only ─────────────

BEZEL_TEST(builtin_closed_ctor_surface_2994,
           parameterless_exceptioninfo_and_readonlystringspan_are_rejected_spec_17_7) {
    BEZEL_EXPECT_TRUE(rejectsPositionalOnlyArm(newLocal("new ExceptionInfo()")));
    BEZEL_EXPECT_TRUE(
        rejectsPositionalOnlyArm(newLocal("new ReadOnlyStringSpan()")));
}

BEZEL_TEST(builtin_closed_ctor_surface_2994,
           the_declared_positional_constructors_are_not_rejected_spec_17_7) {
    // §17.7 keeps them: "For a builtin record whose only declared constructor
    // is its positional primary constructor … the UNDECLARED PARAMETERLESS form
    // `new T()` is rejected". The oracle is the ABSENCE of the §17.7
    // diagnostic, not a value — see the KNOWN GAP below for why.
    const std::string ei = newLocal("new ExceptionInfo(\"T\", \"M\", \"S\")");
    const std::string rs =
        newLocal("new ReadOnlyStringSpan(\"abcdef\", 1, 3)");
    BEZEL_EXPECT_FALSE(rejectsPositionalOnlyArm(ei));
    BEZEL_EXPECT_FALSE(rejectsNoCtorArm(ei));
    BEZEL_EXPECT_FALSE(rejectsPositionalOnlyArm(rs));
    BEZEL_EXPECT_FALSE(rejectsNoCtorArm(rs));
    BEZEL_EXPECT_TRUE(runAot(ei).ran);
    BEZEL_EXPECT_TRUE(runAot(rs).ran);
}

BEZEL_TEST(builtin_closed_ctor_surface_2994,
           the_declared_positional_ctor_now_constructs_spec_17_7) {
    // ⚠ WAS `KNOWN_GAP_the_declared_positional_ctor_still_degrades_spec_17_7`,
    // and this is the day it flipped — fix #3098 grew `lowerNew`'s arms for the
    // two record-shaped builtins, so §17.7's KEPT spelling now constructs
    // instead of reaching #2434's zero-init fallback.
    //
    // What was pinned here (measured on 663a68e, #3098's own base): even the
    // DECLARED constructor dropped every supplied string, so
    // `Console.WriteLine(e.TypeName)` printed an EMPTY line, and reading the
    // same field through a CONCATENATION — `"[" + e.TypeName + "]"` — exited
    // 139 (SIGSEGV) on the null `bz_string*`. That was the #2922-class value
    // the #2994 ruling expected arm 2 to make unspellable; arm 2 closed the
    // PARAMETERLESS spelling of it, and #3098 closed this one.
    //
    // The full behavioural grid — both types, the user-record twin §17.7 names
    // as the reference, named arguments, and the crashing concatenation — lives
    // in tests/codegen/builtin_record_positional_ctor_3098_test.cpp. What stays
    // asserted HERE is the pin's own subject: the declared positional form is
    // accepted by arm 2 AND produces the supplied value, so the two halves of
    // §17.7 cannot drift apart unnoticed.
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var e := new ExceptionInfo(\"TypeName\", \"M\", \"S\"); "
        "  Console.WriteLine(e.TypeName); "
        "  Console.WriteLine(\"done\"); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("done") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("TypeName") != std::string::npos);
}

// ── ACCEPT: every declared PRODUCER of the rejected types ──────────────────

BEZEL_TEST(builtin_closed_ctor_surface_2994,
           the_declared_guid_producers_still_yield_their_values_stdlib_2_6) {
    // The value `new Guid()` used to reach by accident is still reachable — by
    // the two spellings §17.7's diagnostic names. `Empty` is "the all-zero UUID
    // — the same value `default(Guid)` produces" (stdlib §2.6).
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var a := Guid.Empty; "
        "  var b := default(Guid); "
        "  Console.WriteLine(a.ToString()); "
        "  Console.WriteLine(b.ToString()); "
        "  return a.ToString().Length + b.ToString().Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(72, r.exitCode);                       // 36 + 36
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("00000000-0000-0000-0000-000000000000") !=
        std::string::npos);
}

BEZEL_TEST(builtin_closed_ctor_surface_2994,
           newguid_and_parse_still_work_stdlib_2_6) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var a := Guid.NewGuid(); "
        "  var b := Guid.Parse(\"00000000-0000-0000-0000-000000000000\"); "
        "  return a.ToString().Length + b.ToString().Length - 36; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(36, r.exitCode);
}

BEZEL_TEST(builtin_closed_ctor_surface_2994,
           the_declared_intptr_producers_still_work_stdlib_10_0) {
    // `IntPtr.Zero`, `default(IntPtr)`, and the §5.2 explicit CONVERSION
    // `IntPtr(longValue)` — a CallExpr, not a `new`, which is exactly why the
    // rejection is keyed on the NewExpr node and not on the type name alone.
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var a := IntPtr.Zero; "
        "  var b := default(IntPtr); "
        "  var c := IntPtr(7L); "
        "  if a = b then return int(long(c)); end; "
        "  return 99; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(7, r.exitCode);
}

BEZEL_TEST(builtin_closed_ctor_surface_2994,
           the_real_readonlystringspan_producer_still_works_stdlib_3_3) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var s := \"abcdef\".AsSpan(1, 3); "
        "  Console.WriteLine(s.ToString()); "
        "  return s.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(3, r.exitCode);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("bcd") != std::string::npos);
}

// ── ACCEPT: §14.2's generic-body equivalence is EXPLICITLY unaffected ──────

BEZEL_TEST(builtin_closed_ctor_surface_2994,
           generic_new_T_at_a_guid_substitution_is_still_admitted_spec_14_2) {
    // §17.7 says so outright: "§14.2's generic-body equivalence … is
    // unaffected: `Guid` satisfies `new()` — its `default` is well-defined — so
    // generic factories still mint empty Guids." The generic body writes
    // `new T()`, whose head name is `T`, so it never reaches the name-keyed
    // arms at all.
    //
    // The oracle is compile-acceptance and the absence of the §17.7 diagnostic,
    // not the produced value: observing it would require `ToString()` on a
    // generic `T` receiver, which this compiler does not implement (#2457).
    const std::string program =
        "module M; "
        "class G public static method Count<T>(): int where T: new() "
        "  var t := new T(); return 1; end Count; end G; "
        "class C public static method Main(): int "
        "  return G.Count<Guid>() + G.Count<DateTime>(); end Main; end C;";
    BEZEL_EXPECT_FALSE(rejectsNoCtorArm(program));
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(2, r.exitCode);
}

BEZEL_TEST(builtin_closed_ctor_surface_2994,
           a_type_parameter_shadowing_a_builtin_name_is_not_claimed_spec_14_2) {
    // A generic type parameter and a reserved builtin have the SAME TypeRef
    // shape (Named, `declaration == nullptr`), so a parameter literally NAMED
    // `Guid` would be indistinguishable by shape. It is excluded by name.
    const std::string program =
        "module M; "
        "class Box<Guid> "
        "  public static method Make(): int where Guid: new() "
        "    var t := new Guid(); return 5; end Make; "
        "end Box; "
        "class C public static method Main(): int "
        "  return Box<int>.Make(); end Main; end C;";
    BEZEL_EXPECT_FALSE(rejectsNoCtorArm(program));
}

// ── ACCEPT: a USER type sharing a builtin's name still constructs ──────────

BEZEL_TEST(builtin_closed_ctor_surface_2994,
           a_user_class_named_guid_still_constructs_spec_11_3) {
    // A user type carries a real `decl` (or, imported, a non-empty
    // `moduleName`), and both are excluded before the name is consulted —
    // exactly as in codegen's `unconstructedBuiltinNewName` twin. Oracle is the
    // VALUE, so a construction that silently degraded would fail here.
    auto r = runAot(
        "module M; "
        "class Guid public readonly property N: int := 7; end Guid; "
        "class C public static method Main(): int "
        "  using var g := new Guid(); return g.N; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(7, r.exitCode);
}

// ── ACCEPT: an identically-shaped USER record is unchanged in both directions ─

BEZEL_TEST(builtin_closed_ctor_surface_2994,
           an_identically_shaped_user_record_behaves_the_same_spec_17_7_22_5) {
    // §17.7's arm 2 is written as an equivalence — "rejected the same way —
    // exactly as for an identically-shaped user record" — so the reference
    // behaviour must be unchanged in BOTH directions. It is §22.5's
    // required-parameter rule that supplies it, and this fix does not touch it.
    const std::string decls =
        "module M; record UR(A: string, B: string, C: string); ";
    auto pos = runAot(
        decls +
        "class C public static method Main(): int "
        "  var r := new UR(\"a\", \"bb\", \"ccc\"); "
        "  return r.A.Length + r.B.Length + r.C.Length; end Main; end C;");
    BEZEL_EXPECT_TRUE(pos.ran);
    BEZEL_EXPECT_EQ(6, pos.exitCode);
    // The parameterless form: still §22.5's own diagnostic, not §17.7's.
    const std::string none =
        decls +
        "class C public static method Main(): int "
        "  var r := new UR(); return 0; end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(none, "required parameter `A` is not supplied"));
    BEZEL_EXPECT_FALSE(rejectsPositionalOnlyArm(none));
    // A BODY record with no positional fields keeps its parameterless form.
    auto body = runAot(
        "module M; record BR public N: int := 3; end BR; "
        "class C public static method Main(): int "
        "  var r := new BR(); return r.N; end Main; end C;");
    BEZEL_EXPECT_TRUE(body.ran);
    BEZEL_EXPECT_EQ(3, body.exitCode);
}

// ── ACCEPT: the REST of kNonHeapReservedBuiltins is deliberately untouched ──

BEZEL_TEST(builtin_closed_ctor_surface_2994,
           other_value_shaped_builtins_keep_their_constructors_spec_17_7) {
    // The ruling names two populations and no more. `DateTime` is a member of
    // the same `kNonHeapReservedBuiltins` table the two arms are carved out of,
    // and declares real constructors — a fix keyed on the whole table instead
    // of on §17.7's two enumerated sets would have rejected this. `Range<T>`
    // is produced by `..` (§16) and `OwningBuffer<T>` by its sized constructor.
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var dt := new DateTime(2024, 1, 2); "
        "  using var buf := new OwningBuffer<int>(4); "
        "  var rg := 0..5; "
        "  return dt.Day + rg.End!; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(7, r.exitCode);                        // 2 + 5
}

BEZEL_TEST(builtin_closed_ctor_surface_2994,
           the_2974_pointer_shaped_gate_is_untouched_spec_11_7) {
    // #2974's own population reaches the CODEGEN gate, which this fix does not
    // change: sema's arms are keyed on two names apiece, so a static class
    // still produces #2974's "produced no value" diagnostic and not §17.7's.
    BEZEL_EXPECT_TRUE(
        hasError(newLocal("new Domain(\"d\")"), "produced no value"));
    BEZEL_EXPECT_FALSE(rejectsNoCtorArm(newLocal("new Domain(\"d\")")));
}
