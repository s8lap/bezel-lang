// Regression tests for issue #2444 — sibling silent-fallthrough categories to
// #2423/#2433: an unresolved member access on the builtin BORROWING iterator
// sentinels `IEnumerator<T>`/`IAsyncEnumerator<T>` (stdlib §4.12) and on the
// builtin dictionary-entry sentinels `DictionaryEntry<K, V>` (stdlib §4.8,
// a record) / `DrainedEntry<K, V>` (stdlib §4.9, a class implementing
// `IDisposable`) compiled with ZERO diagnostic before this fix — confirmed
// live on origin/main: `e.TotallyBogusEnumeratorMember` on an `IEnumerator<T>`
// and `entry.TotallyBogusEntryMember` on a `for var entry in dict do` element
// both compiled at exit 0 with no diagnostic text on stdout/stderr.
//
// Root cause: identical shape to #2423. `ExprTC::resolveMemberAccess`
// (src/sema/ExprTypecheck_Calls.cpp) special-cases member access on these
// declaration-less builtin Named types (`declaration == nullptr`) by matching
// the member NAME against hand-written cases (`Current` for the enumerators;
// `Key`/`Value` for the entries). A name matching none of them fell all the
// way through this function's tail `return;` with no diagnostic ever emitted
// and `ma`'s exprType left unset.
//
// The fix adds two new reject blocks at the tail of `resolveMemberAccess`'s
// `!targetType` branch (immediately after #2423's exception-type block),
// mirroring #2423's structure exactly: for `IEnumerator<T>`/
// `IAsyncEnumerator<T>` receivers (detected the same way the pre-existing
// `.Current` handler detects them, a few hundred lines up) and for
// `DictionaryEntry<K, V>`/`DrainedEntry<K, V>` receivers (detected via the
// same `importedName` tag the pre-existing `.Key`/`.Value` handler uses).
//
// #2433 (a sibling attempt at the *type-parameter* category, NOT this
// issue's categories) was retracted after it preempted #590's deliberate
// codegen-deferred (`genericSubstSemaTy_`) by-name member resolution for a
// genuinely-declared generic type parameter. That hazard does not apply
// here: `IEnumerator<T>`/`IAsyncEnumerator<T>`/`DictionaryEntry<K, V>`/
// `DrainedEntry<K, V>` are fixed builtin synthetic types, never declared
// type parameters, so there is no deferred per-instantiation resolution
// step to preempt — the entire member surface is enumerable right at this
// sentinel's own declaration, exactly like #2423's exception types.
//
// This issue ALSO named a third sibling category — `Dictionary<K,
// V>.Keys`/`.Values` (typed by sema as a synthetic `Array<K>`/`Array<V>`
// Named/no-declaration TypeRef) — NOT fixed here. Investigation for this fix
// found the gap there is NOT specific to Keys/Values: a bogus member on a
// DIRECTLY-declared `Array<int>` local (`arr.TotallyBogusArrayMember123`, no
// Dictionary involved at all) ALSO compiles at exit 0 with zero diagnostics
// on current main — the SAME silent-fallthrough shape, but for the much
// larger, heavily-used `Array<T>`/`OwningArray<T>`/`Stack<T>`/`Queue<T>`/
// `Set<T>`/`LinkedList<T>`/`Span<T>`/... builtin-collection family as a
// whole, not a Keys/Values-specific gap. Safely rejecting that requires
// enumerating each collection's COMPLETE stdlib member surface (§4.3-§4.16)
// and confirming no §26.5 extension-method mechanism depends on the
// silence — a materially larger audit than this fix's other two categories,
// which have small, closed (2-3 member) surfaces. Filed separately
// (issue #2457) rather than attempted here, per the project's
// decline-with-reasons discipline; this file carries no regression test for
// that category.
//
// ── #2458/#2459 follow-up (this revision) ──────────────────────────────
// Verifying the universal Object members (ToString/Equals/GetHashCode —
// §8.1) stayed legitimately exempt (not newly rejected by the block above)
// surfaced two SEPARATE, pre-existing bugs, unrelated to the diagnostics
// gap this file otherwise covers:
//   - #2458: `ToString()` on IEnumerator<T>/DictionaryEntry<K,V> crashed
//     (codegen misrouted the call through `lowerObjectToString`'s
//     builtin-EXCEPTION vtable-slot-3 dispatch — a guard keyed on
//     `declaration == nullptr` alone, which also (wrongly) matches every
//     other declaration-less builtin type). Fixed: narrowed the guard to
//     `isBuiltinExceptionTypeRef` (the precise, pre-existing, name-checked
//     helper LlvmBackend_impl.hpp already defines for exactly this
//     purpose) at both call sites.
//   - #2459 (tracked as its own follow-up issue): tracing what SHOULD
//     happen once the crash stopped found `Equals`/`GetHashCode` on
//     `IEnumerator<T>`/`IAsyncEnumerator<T>` were simply never dispatched
//     at all (silently returning 0/false) — but these receivers ARE
//     genuine heap-allocated objects, so the §8.3 "Class" default
//     (reference equality / identity hash) applies mechanically; IMPLEMENTED
//     via direct pointer identity + the existing generic
//     `bz_obj_identity_hash` runtime helper. `DrainedEntry<K,V>.ToString()`
//     is similarly IMPLEMENTED as the fixed literal "DrainedEntry" (stdlib
//     §4.9 documents it as a real class; §8.5's class default is a pure
//     compile-time constant, the type's qualified name). The REMAINDER —
//     `IEnumerator<T>`/`IAsyncEnumerator<T>.ToString()` (no nameable
//     runtime type for the internal iterator wrapper), and
//     `DictionaryEntry<K,V>`'s full Object-member trio plus
//     `DrainedEntry<K,V>.Equals()`/`.GetHashCode()` (both need per-field or
//     real-heap-identity machinery this fix's budget does not cover
//     safely) — gets an honest sema-level "declared ... but not yet
//     implemented (tracked #2459)" diagnostic, mirroring #2423's own
//     SerializationException.Offset precedent, rather than an invented
//     implementation (CLAUDE.md rule 2) or the pre-#2458 silent 0/false/
//     crash. The two tests below originally named
//     "legit_universal_object_members_not_regressed" for each group
//     predate this discovery and asserted only "compiles, exit 0" for
//     GetHashCode/Equals on EVERY one of these receivers; they are updated
//     here to match the now-verified-correct behavior instead — a
//     legitimate test correction, not a milestone-scope violation.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Returns true iff the run produced an Error diagnostic containing `keyword`.
// Per tests/README.md: runAot's WIFSIGNALED path also synthesizes an Error
// diagnostic for a runtime crash, so a genuine compile-time rejection must be
// distinguished by asserting on the diagnostic TEXT (this helper), never by
// exit code or `!r.ran` alone — mirrors tests/codegen/
// exception_unresolved_member_2423_test.cpp's identical helper.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// ═════════════════ Group 1: IEnumerator<T>/IAsyncEnumerator<T> ═════════════

// #2444 core repro (property-read form, sync IEnumerator<T>): confirmed on
// origin/main to compile at exit 0 with zero diagnostics before this fix.
BEZEL_TEST(iter_enumerator_unresolved_member_2444,
           bogus_property_on_ienumerator_rejected_stdlib_4_12) {
    const std::string program =
        "module M; "
        "class C public static method F(a: Array<int>): int "
        "  var e := a.GetEnumerator(); "
        "  var x := e.TotallyBogusEnumeratorMember; "
        "  e.Dispose(); "
        "  return 0; "
        "end F; "
        "public static method Main(): int return 0; end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "no member `TotallyBogusEnumeratorMember` on builtin "
        "`IEnumerator<T>`"));
}

// Call-expression form (`e.Bogus()`) — the callee is the same
// MemberAccessExpr shape; must be rejected identically.
BEZEL_TEST(iter_enumerator_unresolved_member_2444,
           bogus_call_form_on_ienumerator_rejected) {
    const std::string program =
        "module M; "
        "class C public static method F(a: Array<int>): int "
        "  var e := a.GetEnumerator(); "
        "  var x := e.Bogus(); "
        "  e.Dispose(); "
        "  return 0; "
        "end F; "
        "public static method Main(): int return 0; end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(program, "no member `Bogus` on builtin `IEnumerator<T>`"));
}

// Async variant (IAsyncEnumerator<T>, reached via an async-iterator method
// return type) — confirms the fix is not special-cased to the sync form.
BEZEL_TEST(iter_enumerator_unresolved_member_2444,
           bogus_property_on_iasyncenumerator_rejected_stdlib_4_12) {
    const std::string program =
        "module M; "
        "class C public static async method Gen(): IAsyncEnumerator<int> "
        "  yield return 1; "
        "end Gen; "
        "public static async method Main(): Task<int> "
        "  await using var en := Gen(); "
        "  var x := en.TotallyBogusAsyncMember; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "no member `TotallyBogusAsyncMember` on builtin "
        "`IAsyncEnumerator<T>`"));
}

// ── Legitimate members keep working (no false rejects) ──────────────────

// Current + MoveNext(): the documented core of the manual protocol
// (stdlib §4.12). VALUE-correctness, not just clean compile: sums the
// array through the manual enumerator protocol and returns the sum.
BEZEL_TEST(iter_enumerator_unresolved_member_2444,
           legit_current_and_movenext_value_correct) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a := new Array<int>(3); "
        "  a.Add(3); a.Add(4); a.Add(5); "
        "  var e := a.GetEnumerator(); "
        "  var total := 0; "
        "  while e.MoveNext() do "
        "    total := total + e.Current; "
        "  end; "
        "  e.Dispose(); "
        "  a.Dispose(); "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 12);
}

// MoveNextAsync() + Current on IAsyncEnumerator<T> — the async counterpart,
// value-correct (mirrors tests/codegen/async_iter_manual_protocol_759_test.cpp's
// established repro shape as a direct guard on this fix's exempt set).
BEZEL_TEST(iter_enumerator_unresolved_member_2444,
           legit_movenextasync_and_current_value_correct) {
    const std::string program =
        "module M; "
        "class C public static async method Gen(): IAsyncEnumerator<int> "
        "  yield return 42; "
        "  yield return 99; "
        "end Gen; "
        "public static async method Main(): Task<int> "
        "  await using var en := Gen(); "
        "  var m1 := await en.MoveNextAsync(); "
        "  if not m1 then return 1; end; "
        "  if en.Current <> 42 then return 2; end; "
        "  var m2 := await en.MoveNextAsync(); "
        "  if not m2 then return 3; end; "
        "  if en.Current <> 99 then return 4; end; "
        "  var m3 := await en.MoveNextAsync(); "
        "  if m3 then return 5; end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 0);
}

// Explicit `.Dispose()` on the sync sentinel — heavily tested elsewhere
// (tests/codegen/array_getenumerator_1002_test.cpp,
// tests/codegen/iter_second_dispose_idempotent_1107_test.cpp); this guards
// specifically against THIS fix's new reject firing on it.
BEZEL_TEST(iter_enumerator_unresolved_member_2444,
           legit_explicit_dispose_not_regressed) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a := new Array<int>(1); "
        "  a.Add(7); "
        "  var e := a.GetEnumerator(); "
        "  e.Dispose(); "
        "  e.Dispose(); "  // idempotent, per #1107
        "  a.Dispose(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 0);
}

// #2459: `GetHashCode`/`Equals` on the sync sentinel are IMPLEMENTED (direct
// reference identity + the generic bz_obj_identity_hash runtime helper —
// language spec §8.3 "Class" default) — VALUE-correct, not merely "doesn't
// crash": self-equality is true, two INDEPENDENT enumerators (from separate
// GetEnumerator() calls) compare unequal, and a single object's hash is
// STABLE across repeated calls.
BEZEL_TEST(iter_enumerator_unresolved_member_2444,
           equals_and_gethashcode_identity_semantics_spec_8_3) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var a := new Array<int>(2); "
        "  a.Add(1); a.Add(2); "
        "  var e1 := a.GetEnumerator(); "
        "  var e2 := a.GetEnumerator(); "
        "  var selfEq := e1.Equals(e1); "
        "  var crossEq := e1.Equals(e2); "
        "  var h1a := e1.GetHashCode(); "
        "  var h1b := e1.GetHashCode(); "
        "  e1.Dispose(); e2.Dispose(); a.Dispose(); "
        "  if not selfEq then return 1; end; "
        "  if crossEq then return 2; end; "        // distinct objects, distinct identity
        "  if h1a <> h1b then return 3; end; "      // stable across repeated calls
        "  return 42; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}

// #2458/#2459: `ToString()` on the sync sentinel — before #2458's fix this
// crashed at runtime (codegen misrouted the call through the builtin-
// exception ToString vtable-slot-3 dispatch onto the DIFFERENTLY-SHAPED
// §29.11 iterator vtable's own slot 3, `Current`, called as if it were a
// `ptr(ptr)` ToString thunk). Confirmed live on origin/main before this fix
// (with the sema-level exemption that let the call reach codegen at all):
// `bezelc -o` compiled at exit 0, running the binary segfaulted. Now
// rejected at compile time instead, with a distinct "declared but not yet
// implemented" diagnostic (#2459) rather than either the crash or an
// invented qualified-name literal (there is none defensible for this
// receiver's unnamed internal runtime type — see #2459's own analysis).
BEZEL_TEST(iter_enumerator_unresolved_member_2444,
           tostring_declared_not_implemented_ienumerator_2459) {
    const std::string program =
        "module M; "
        "class C public static method F(a: Array<int>): string "
        "  var e := a.GetEnumerator(); "
        "  var s := e.ToString(); "
        "  e.Dispose(); "
        "  return s; "
        "end F; "
        "public static method Main(): int return 0; end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "member `ToString` on builtin `IEnumerator<T>` is declared "
        "(language spec \xC2\xA7""8.1/\xC2\xA7""8.5) but is not yet "
        "implemented"));
    BEZEL_EXPECT_TRUE(hasError(program, "#2459"));
}

// Async variant of the same ToString rejection.
BEZEL_TEST(iter_enumerator_unresolved_member_2444,
           tostring_declared_not_implemented_iasyncenumerator_2459) {
    const std::string program =
        "module M; "
        "class C public static async method Gen(): IAsyncEnumerator<int> "
        "  yield return 1; "
        "end Gen; "
        "public static async method Main(): Task<int> "
        "  await using var en := Gen(); "
        "  var s := en.ToString(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "member `ToString` on builtin `IAsyncEnumerator<T>` is declared"));
}

// ═════════════════ Group 2: DictionaryEntry<K,V>/DrainedEntry<K,V> ════════

// #2444 core repro: `entry.TotallyBogusEntryMember` on the `for var entry in
// dict do` element type (DictionaryEntry<K,V>) — confirmed on origin/main to
// compile at exit 0 with zero diagnostics before this fix.
BEZEL_TEST(dictionary_entry_unresolved_member_2444,
           bogus_member_on_dictionaryentry_rejected_stdlib_4_8) {
    const std::string program =
        "module M; "
        "class C public static method F(d: Dictionary<string, int>): int "
        "  for var e in d do "
        "    return e.TotallyBogusEntryMember; "
        "  end; "
        "  return 0; "
        "end F; "
        "public static method Main(): int return 0; end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "no member `TotallyBogusEntryMember` on `DictionaryEntry<K, V>`"));
}

// DrainedEntry<K,V> (owning dictionary's Drain() element type) — same shape,
// different sentinel tag; confirms the fix is not special-cased to the
// borrowing entry.
BEZEL_TEST(dictionary_entry_unresolved_member_2444,
           bogus_member_on_drainedentry_rejected_stdlib_4_9) {
    const std::string program =
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(tag: int) Tag := tag; end constructor; "
        "end Res; "
        "class C public static method Main(): int "
        "  var d := new OwningDictionary<string, Res>(); "
        "  d.Add(\"a\", new Res(1)); "
        "  for var entry in d.Drain() do "
        "    using var e := owned entry; "
        "    var x := e.TotallyBogusDrainedMember; "
        "  end; "
        "  d.Dispose(); "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(hasError(
        program,
        "no member `TotallyBogusDrainedMember` on `DrainedEntry<K, V>`"));
}

// `Dispose` is NOT on the documented DictionaryEntry surface: it is a
// RECORD, and language spec §8.7 "Dispose is not on Object" / records never
// implement IDisposable. A bogus `.Dispose()` call must stay REJECTED, not
// silently exempted the way it legitimately is for DrainedEntry (a class).
BEZEL_TEST(dictionary_entry_unresolved_member_2444,
           dispose_on_dictionaryentry_rejected_records_no_idisposable) {
    const std::string program =
        "module M; "
        "class C public static method F(d: Dictionary<string, int>): int "
        "  for var e in d do "
        "    e.Dispose(); "
        "  end; "
        "  return 0; "
        "end F; "
        "public static method Main(): int return 0; end Main; end C;";
    BEZEL_EXPECT_TRUE(
        hasError(program, "no member `Dispose` on `DictionaryEntry<K, V>`"));
}

// ── Legitimate members keep working (no false rejects), VALUE-correct ────

// Key/Value roundtrip on the borrowing DictionaryEntry — the pre-existing
// #548 handler this fix sits behind; guards the fix does not regress it.
BEZEL_TEST(dictionary_entry_unresolved_member_2444,
           legit_key_value_roundtrip_dictionaryentry) {
    const std::string program =
        "module M; "
        "class C public static method Main(): int "
        "  var d := new Dictionary<string, int>(); "
        "  d[\"x\"] := 41; "
        "  var total := 0; "
        "  for var e in d do "
        "    if e.Key = \"x\" and e.Value = 41 then total := 100; end; "
        "  end; "
        "  d.Dispose(); "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 100);
}

// Key/Value + explicit Dispose() on DrainedEntry — ownership-transferring
// drain (stdlib §4.9); VALUE-correct (returns the drained Tag).
BEZEL_TEST(dictionary_entry_unresolved_member_2444,
           legit_key_value_dispose_drainedentry) {
    const std::string program =
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(tag: int) Tag := tag; end constructor; "
        "end Res; "
        "class C public static method Main(): int "
        "  var d := new OwningDictionary<string, Res>(); "
        "  d.Add(\"k\", new Res(37)); "
        "  var total := 0; "
        "  for var entry in d.Drain() do "
        "    if entry.Key = \"k\" then total := entry.Value.Tag; end; "
        "    entry.Dispose(); "
        "  end; "
        "  d.Dispose(); "
        "  return total; "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 37);
}

// #2459: DictionaryEntry<K,V>'s full Object-member trio (ToString/Equals/
// GetHashCode) is genuinely declared (§8.1/§8.3/§8.5) but NOT implemented —
// it is a positional RECORD, so its documented default is FIELD-BY-FIELD
// (needs K's/V's own recursive ToString/Equals/GetHashCode dispatch against
// a synthetic type with no TypeSymbol to derive the field layout from
// generically), unlike IEnumerator<T>'s simple reference-identity default.
// Before #2458/#2459: ToString crashed (same misrouting bug as
// IEnumerator's), Equals/GetHashCode silently returned false/0. Now all
// three are honest compile-time rejections instead — this test replaces
// the prior "legit_universal_object_members_not_regressed" test, which
// asserted (incorrectly, discovered during #2458's investigation) that
// these three silently compiled.
BEZEL_TEST(dictionary_entry_unresolved_member_2444,
           tostring_equals_gethashcode_declared_not_implemented_2459) {
    // `callExpr` is the full call text (e.g. "ToString()" / "Equals(e)") so
    // Equals can be exercised with its real one-argument signature.
    auto rejects = [](const char* mbr, const char* callExpr) {
        std::string program =
            std::string("module M; ") +
            "class C public static method F(d: Dictionary<string, int>): int "
            "  for var e in d do "
            "    var x := e." + callExpr + "; "
            "  end; "
            "  return 0; "
            "end F; "
            "public static method Main(): int return 0; end Main; end C;";
        BEZEL_EXPECT_TRUE(hasError(
            program, (std::string("member `") + mbr +
                          "` on `DictionaryEntry<K, V>` is declared").c_str()));
        BEZEL_EXPECT_TRUE(hasError(program, "#2459"));
    };
    rejects("ToString", "ToString()");
    rejects("Equals", "Equals(e)");
    rejects("GetHashCode", "GetHashCode()");
}

// #2459: DrainedEntry<K,V>.Equals()/.GetHashCode() are ALSO declared-not-
// implemented, but for a DIFFERENT reason than DictionaryEntry: despite
// being documented as a `class` (stdlib §4.9), its actual codegen
// representation is a plain value-struct `{K key; V* value}`
// (LlvmBackend_Stmts.cpp ~3643), not a real heap object — so, unlike
// IEnumerator<T> (a genuine heap allocation), the §8.3 class default
// (reference identity) cannot be soundly implemented against it.
BEZEL_TEST(dictionary_entry_unresolved_member_2444,
           equals_gethashcode_declared_not_implemented_drainedentry_2459) {
    auto rejects = [](const char* mbr, const char* callExpr) {
        std::string program =
            std::string("module M; ") +
            "class Res public Tag: int; "
            "  public constructor(tag: int) Tag := tag; end constructor; "
            "end Res; "
            "class C public static method Main(): int "
            "  var d := new OwningDictionary<string, Res>(); "
            "  d.Add(\"a\", new Res(1)); "
            "  for var entry in d.Drain() do "
            "    using var e := owned entry; "
            "    var x := e." + callExpr + "; "
            "  end; "
            "  d.Dispose(); "
            "  return 0; "
            "end Main; end C;";
        BEZEL_EXPECT_TRUE(hasError(
            program, (std::string("member `") + mbr +
                          "` on `DrainedEntry<K, V>` is declared").c_str()));
        BEZEL_EXPECT_TRUE(hasError(program, "#2459"));
    };
    rejects("Equals", "Equals(e)");
    rejects("GetHashCode", "GetHashCode()");
}

// #2459: DrainedEntry<K,V>.ToString() IS implemented (unlike Equals/
// GetHashCode above) — stdlib §4.9 documents it as a real `class`, and
// §8.5's class default ("the type's qualified name") is a pure compile-time
// constant that never reads the receiver's runtime value, so DrainedEntry's
// value-struct representation (unlike for Equals/GetHashCode) is no
// obstacle. VALUE-correct: asserts the literal text, not just a clean
// compile.
BEZEL_TEST(dictionary_entry_unresolved_member_2444,
           tostring_implemented_drainedentry_literal_2459) {
    // Main returns `int` (not `string`, which does not round-trip through
    // the 8-bit process exit code — tests/README.md): compare the
    // ToString() result to the expected literal and encode the outcome as
    // a sentinel exit code.
    const std::string program =
        "module M; "
        "class Res public Tag: int; "
        "  public constructor(tag: int) Tag := tag; end constructor; "
        "end Res; "
        "class C public static method Main(): int "
        "  var d := new OwningDictionary<string, Res>(); "
        "  d.Add(\"a\", new Res(1)); "
        "  var result := \"\"; "
        "  for var entry in d.Drain() do "
        "    using var e := owned entry; "
        "    result := e.ToString(); "
        "  end; "
        "  d.Dispose(); "
        "  return (if result = \"DrainedEntry\" then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 42);
}
