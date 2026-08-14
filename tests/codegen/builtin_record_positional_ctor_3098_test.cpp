// Spec-derived regression test for issue #3098 — THE DECLARED POSITIONAL
// CONSTRUCTORS OF THE RECORD-SHAPED BUILTINS (language spec §17.7 "Standard-
// library builtins have a closed constructor surface", v0.241; §31.4;
// stdlib §3.3).
//
// §17.7, verbatim, on the arm this file covers:
//
//   "For a builtin record whose only declared constructor is its positional
//    primary constructor (`ExceptionInfo`, §31.4; `ReadOnlyStringSpan`, stdlib
//    §3.3), the undeclared parameterless form `new T()` is rejected the same
//    way — exactly as for an identically-shaped user record."
//
// #2994 implemented that REJECTION (see
// tests/codegen/builtin_closed_ctor_surface_2994_test.cpp). The SAME sentence
// keeps the positional form: it is the constructor the standard library
// declares, so it must actually construct.
//
// ── WHAT IT REPLACES (measured on 663a68e, the parent) ─────────────────────
//
// `lowerNew` had no arm for either type, returned nullptr, and the construction
// fell through to `lowerLocalDecl`'s #2434 zero-init fallback:
//
//   var e := new ExceptionInfo("TypeName", "M", "S");
//   Console.WriteLine(e.TypeName);       →  an EMPTY line, exit 0, behind
//                                            `internal: … (degraded to a
//                                            zero-initialized value) — #2434`
//
//   Console.WriteLine("[" + e.TypeName + "]");   →  exit 139 (SIGSEGV): the
//                                                    null `bz_string*` is
//                                                    dereferenced
//
// `new ReadOnlyStringSpan("abcdef", 1, 3)` degraded identically (all-zero
// `{ptr Source, i32 Start, i32 Length}` — a null source and a zero length).
//
// This is the #2922 class exactly — a silent wrong value escalating to a crash
// — and it is reachable by the DOCUMENTED spelling: §31.4 declares
// `ExceptionInfo` as public API and it is the element type of
// `Exception.CauseChain`, while stdlib §3.3 defines `ReadOnlyStringSpan.Slice`
// as `new ReadOnlyStringSpan(Source, this.Start + start, length)` — the
// standard library's own text constructing one this way.
//
// ── THE REFERENCE BEHAVIOUR ────────────────────────────────────────────────
//
// §17.7 names it: "exactly as for an identically-shaped user record". The user
// twin worked on the parent and is asserted below beside each builtin, so the
// oracle is a property of the language rather than a hand-written expectation.
//
// ── ANTI-OVER-REACH ────────────────────────────────────────────────────────
//
// The fix must not re-open what #2994 closed: the UNDECLARED parameterless form
// stays rejected, and the two `kNoConstructorBuiltins` (`Guid`, `IntPtr`) stay
// rejected at every argument list. Both are re-asserted here so that a lane
// touching `lowerNew` sees the two halves of §17.7 in one place.
//
// ── WHERE THE "NO `internal:` RESIDUAL" ORACLE LIVES ──────────────────────
//
// NOT here. `runAot` populates `JitResult::diagnostics` only on a compile
// REJECT (or a compiler crash/timeout) — the #2434 Warning this fix removes is
// emitted by a program that COMPILES, so an assertion about it in this file
// would be structurally incapable of failing. The silence oracle is
// tests/lit/builtin_record_positional_ctor_accept_3098.bzl, which pipes the
// real `bezelc` stderr through FileCheck.
//
// ⚠ NO EAGER RANGE CHECK is asserted for `ReadOnlyStringSpan`, deliberately:
// stdlib §4.14 "Construction patterns" states that "the direct constructors of
// the five span records do **not** range-check `(Start, Length)` against the
// source (v0.195): a mis-constructed span faults at each out-of-range *access*
// instead — the span-level check passes and the source's own bounds check
// throws `IndexOutOfRangeException`". Constructing a span is therefore not the
// place a bad range is reported, and this file does not claim it is — but the
// ACCESS-time throw §4.14 promises in the same sentence is the precondition that
// makes the rule memory-safe, it did NOT exist for this type, and it is
// asserted below (`a_misranged_span_throws_at_the_access_not_out_of_bounds…`).
// Recorded on #3135, which named this exact risk for the other four span
// records.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool hasError(const std::string& program, const char* needle) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ── ExceptionInfo (§31.4) ──────────────────────────────────────────────────

BEZEL_TEST(builtin_record_positional_ctor_3098,
           exceptioninfo_positional_ctor_keeps_every_supplied_field_spec_17_7_31_4) {
    // §31.4: `public record ExceptionInfo(TypeName: string, Message: string,
    // StackTrace: string);`
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var e := new ExceptionInfo(\"TypeName\", \"Msg\", \"Stack\"); "
        "  Console.WriteLine(e.TypeName); "
        "  Console.WriteLine(e.Message); "
        "  Console.WriteLine(e.StackTrace); "
        "  return e.TypeName.Length + e.Message.Length + e.StackTrace.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(16, r.exitCode);                       // 8 + 3 + 5
    BEZEL_EXPECT_TRUE(r.stdoutText.find("TypeName") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("Msg") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("Stack") != std::string::npos);
}

BEZEL_TEST(builtin_record_positional_ctor_3098,
           exceptioninfo_field_through_a_concatenation_no_longer_segfaults_spec_31_4) {
    // THE CRASH ARM. Measured on 663a68e: exit 139. A null `string` field is
    // not merely a wrong value — the first `+` dereferences it.
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var e := new ExceptionInfo(\"TypeName\", \"Msg\", \"Stack\"); "
        "  Console.WriteLine(\"[\" + e.TypeName + \"]\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);                        // NOT 139
    BEZEL_EXPECT_TRUE(r.stdoutText.find("[TypeName]") != std::string::npos);
}

BEZEL_TEST(builtin_record_positional_ctor_3098,
           exceptioninfo_matches_its_identically_shaped_user_record_spec_17_7) {
    // §17.7's own reference behaviour — "exactly as for an identically-shaped
    // user record" — asserted as an EQUALITY between the two, so the oracle is
    // the language rule rather than a transcribed expectation.
    auto r = runAot(
        "module M; "
        "record UR(TypeName: string, Message: string, StackTrace: string); "
        "class C public static method Main(): int "
        "  var u := new UR(\"a\", \"bb\", \"ccc\"); "
        "  var e := new ExceptionInfo(\"a\", \"bb\", \"ccc\"); "
        "  var ok: int := 0; "
        "  if e.TypeName = u.TypeName then ok := ok + 1; end; "
        "  if e.Message = u.Message then ok := ok + 2; end; "
        "  if e.StackTrace = u.StackTrace then ok := ok + 4; end; "
        "  return ok; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(7, r.exitCode);
}

BEZEL_TEST(builtin_record_positional_ctor_3098,
           exceptioninfo_named_arguments_bind_by_name_not_position_spec_9_1) {
    // A record's positional constructor takes its FIELD NAMES as parameter
    // names (§9.1), so a permuted named-argument list must produce the same
    // value as the positional spelling. Reading them positionally would be a
    // silent field permutation — the defect class this fix exists to remove.
    // §22.5's named-argument spelling is `name := value`, and named arguments
    // "may appear in any order after positional arguments" — both forms below.
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var e := new ExceptionInfo(Message := \"Msg\", "
        "                             StackTrace := \"Stack\", "
        "                             TypeName := \"TypeName\"); "
        "  var f := new ExceptionInfo(\"T2\", StackTrace := \"S2\", "
        "                             Message := \"M2\"); "
        "  Console.WriteLine(e.TypeName + \"|\" + e.Message + \"|\" + "
        "                    e.StackTrace); "
        "  Console.WriteLine(f.TypeName + \"|\" + f.Message + \"|\" + "
        "                    f.StackTrace); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
    BEZEL_EXPECT_TRUE(
        r.stdoutText.find("TypeName|Msg|Stack") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("T2|M2|S2") != std::string::npos);
}

// ── ReadOnlyStringSpan (stdlib §3.3) ───────────────────────────────────────

BEZEL_TEST(builtin_record_positional_ctor_3098,
           readonlystringspan_positional_ctor_builds_the_declared_view_stdlib_3_3) {
    // stdlib §3.3: `public record ReadOnlyStringSpan(Source: string,
    // Start: int, Length: int): IReadOnlySpan<char>`, with `[i]` returning
    // "the code point at position `Start + i` in the source string" and
    // `ToString()` returning "a `string` containing the span's code points".
    // The composite observable (398) is outside a process exit status's 8 bits,
    // so it travels on stdout — tests/README.md's runAot/runAotOut rule.
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var s := new ReadOnlyStringSpan(\"abcdef\", 1, 3); "
        "  Console.WriteLine(s.ToString()); "
        "  Console.WriteLine(s.Length * 100 + int(s[0])); "
        "  return s.Length; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(3, r.exitCode);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("bcd") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("398") != std::string::npos);  // 'b'==98
}

BEZEL_TEST(builtin_record_positional_ctor_3098,
           readonlystringspan_ctor_agrees_with_the_asspan_factory_stdlib_3_3_9_6) {
    // The factory path (`string.AsSpan(start, length)`) worked on the parent;
    // stdlib §3.3 says the direct constructor is "admitted but rarely needed",
    // i.e. the same value by another spelling. Asserting the AGREEMENT makes
    // the already-working path the oracle.
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var a := \"abcdef\".AsSpan(1, 3); "
        "  var b := new ReadOnlyStringSpan(\"abcdef\", 1, 3); "
        "  var ok: int := 0; "
        "  if a.Length = b.Length then ok := ok + 1; end; "
        "  if a.ToString() = b.ToString() then ok := ok + 2; end; "
        "  if b.SequenceEqual(a) then ok := ok + 4; end; "
        "  return ok; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(7, r.exitCode);
}

// ── ANTI-OVER-REACH: #2994's rejections must still hold ────────────────────

BEZEL_TEST(builtin_record_positional_ctor_3098,
           the_undeclared_parameterless_form_is_still_rejected_spec_17_7) {
    // The half of §17.7 #2994 closed. Growing the positional arm must not
    // re-open it — `new T()` is not a declared constructor of either type.
    for (const char* ctor : {"new ExceptionInfo()", "new ReadOnlyStringSpan()"}) {
        const std::string p =
            std::string("module M; class C public static method Main(): int "
                        "  var x := ") + ctor + "; return 0; end Main; end C;";
        BEZEL_EXPECT_TRUE(hasError(p, "is not a declared constructor of"));
        BEZEL_EXPECT_TRUE(hasError(p, "\xC2\xA7""17.7"));
    }
}

BEZEL_TEST(builtin_record_positional_ctor_3098,
           the_no_constructor_builtins_are_still_rejected_spec_17_7) {
    // §17.7's OTHER arm — `Guid`/`IntPtr`/`UIntPtr` "admit no `new T(...)` at
    // any argument list". Untouched by this fix, asserted so a `lowerNew` edit
    // cannot quietly restore the pre-#2994 accident.
    for (const char* ctor : {"new Guid()", "new IntPtr(0)", "new UIntPtr()"}) {
        const std::string p =
            std::string("module M; class C public static method Main(): int "
                        "  var x := ") + ctor + "; return 0; end Main; end C;";
        BEZEL_EXPECT_TRUE(hasError(p, "is not admitted at any argument list"));
    }
}

BEZEL_TEST(builtin_record_positional_ctor_3098,
           a_wrong_arity_positional_list_is_rejected_at_sema_spec_17_7) {
    // §17.7: "the constructor surface is exactly what that document declares".
    // Neither record declares a 2-argument constructor, so a wrong-arity list
    // must be DECLINED rather than constructed from. Pre-#3136 the program
    // below compiled and reached the value through the #2434 zero-init
    // fallback (a record of NULL strings); #3136 grew sema's §17.7 arm to
    // check the DECLARED ARITY, so the program now fails to compile with the
    // §17.7 wording rather than reaching the value at all — the property
    // stronger than this test originally asserted (which only required the
    // supplied arguments to not reach the value, not the program to fail).
    //
    // Full behavioural grid for the new arity rejection lives in
    // tests/codegen/builtin_record_positional_ctor_arity_3136_test.cpp; this
    // test stays here so the #3098 family owns the spelled-out behaviour at
    // every wrong arity and the flip from "degrades silently" to "rejected
    // at sema" cannot drift unnoticed.
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  Console.WriteLine(\"<start>\"); "
        "  var e := new ExceptionInfo(\"AAA\", \"BBB\"); "
        "  Console.WriteLine(e.TypeName); "
        "  Console.WriteLine(\"<end>\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool hasSec = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("is not a declared constructor of") !=
                std::string::npos &&
            d.message.find("\xC2\xA7""17.7") != std::string::npos)
            hasSec = true;
    BEZEL_EXPECT_TRUE(hasSec);
}

// ── THE ACCESS-TIME GUARD stdlib §4.14 PROMISES ─────────────────────────

BEZEL_TEST(builtin_record_positional_ctor_3098,
           a_misranged_span_throws_at_the_access_not_out_of_bounds_stdlib_3_3_4_14) {
    // ⚠ THE SAFETY PRECONDITION OF THE NO-EAGER-CHECK RULE, made real.
    //
    // stdlib §4.14 "Construction patterns" (v0.195) says the direct span
    // constructors "do **not** range-check `(Start, Length)` against the source
    // … a mis-constructed span faults at each out-of-range *access* instead —
    // the span-level check passes and **the source's own bounds check throws
    // `IndexOutOfRangeException`**". Following that rule (which this fix does)
    // is only memory-safe if that second check EXISTS. For
    // `ReadOnlyStringSpan` it did not: the indexer checked the span's own
    // `Length` and then called `loadCodePoint` directly, whose header states
    // "idx < len is guaranteed by every caller" and which performs no check —
    // so `new ReadOnlyStringSpan("abc", 0, 100)` then `s[50]` read ~50 code
    // points past the buffer. Before the ctor arm existed the construction
    // degraded to an all-zero span and `s[0]` threw on `0 >= 0`: a wrong value,
    // but memory-SAFE. Making the constructor construct is what made the
    // out-of-bounds read constructible, so the guard is owed by the same fix.
    //
    // Both halves of a mis-range are covered: an over-long `Length` (the span
    // check passes, the source check must fire) and a NEGATIVE `Start` (the
    // span check says nothing about `Start`, so only the source check can).
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var over := new ReadOnlyStringSpan(\"abc\", 0, 100); "
        "  var neg  := new ReadOnlyStringSpan(\"abc\", -5, 10); "
        "  var got: int := 0; "
        "  try "
        "    var c := over[50]; "
        "    Console.WriteLine(\"NO THROW over\"); "
        "  catch "
        "    var e: IndexOutOfRangeException => got := got + 1; "
        "  end; "
        "  try "
        "    var c := neg[0]; "
        "    Console.WriteLine(\"NO THROW neg\"); "
        "  catch "
        "    var e: IndexOutOfRangeException => got := got + 2; "
        "  end; "
        "  return got; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(3, r.exitCode);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("NO THROW") == std::string::npos);
}

BEZEL_TEST(builtin_record_positional_ctor_3098,
           an_in_range_span_still_indexes_and_slices_stdlib_3_3) {
    // The accept-control for the guard above: it must fire only on a genuine
    // out-of-source access. stdlib §3.3's indexer reads "the code point at
    // position `Start + i` in the source string", and `Slice(start, length)` is
    // "`new ReadOnlyStringSpan(Source, this.Start + start, length)`" — the
    // standard library's own use of the constructor this fix lowered, and the
    // path by which a mis-range would propagate, so it is exercised here too.
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var s := new ReadOnlyStringSpan(\"abcdef\", 1, 4); "
        "  var t := s.Slice(1, 2); "
        "  Console.WriteLine(s.ToString()); "
        "  Console.WriteLine(t.ToString()); "
        "  return int(t[0]) - int(s[0]); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(1, r.exitCode);               // 'c' (99) - 'b' (98)
    BEZEL_EXPECT_TRUE(r.stdoutText.find("bcde") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("cd") != std::string::npos);
}
