// fix #4235 (§17.11 "The `default` operator" / §14.2 "Constraints" /
// §31.4 "Standard exception types" / stdlib §3.3 "ReadOnlyStringSpan",
// v0.298): `default(ExceptionInfo)` and `default(ReadOnlyStringSpan)`
// compiled clean and reproduced the null-string-deref class this repo
// already fixed for `CString`/`WString`/`RawBytes` (`fix #4166`'s final
// commit) — a SIGSEGV for the former, a silent degrade-to-zero for the
// latter. The equivalent top-level `where T: new()` generic substitution
// (`new T()` at a value substitution is `default(T)`, §14.2) was ALSO
// wrongly accepted for both names.
//
// FIFTH sibling of the #4219/#4223/#4229 null-declaration-builtin
// defect class — see #4229's comments for the full, MEASURED population:
// `builtinTypeArity` (TypeResolution_Names.cpp) names 139 identifiers, of
// which 130 reach a `!declaration` branch with no real `TypeSymbol`; two
// of those 130 — `ExceptionInfo` and `ReadOnlyStringSpan` — are
// VALUE-SHAPED records (not reference types themselves) whose declared
// fields include at least one non-nullable `string`, which per §17.11's
// "defaultable" glossary entry is what makes them non-defaultable: "a
// record any of whose declared fields is not defaultable — a non-nullable
// reference type...".
//
// Quoted in full, the two spec facts this file pins:
//
//   §31.4: "public record ExceptionInfo(TypeName: string, Message: string,
//   StackTrace: string);" — three non-nullable `string` fields.
//
//   stdlib §3.3: "public record ReadOnlyStringSpan(Source: string, Start:
//   int, Length: int): IReadOnlySpan<char>" — `Source` is a non-nullable
//   `string`; one non-defaultable declared field is sufficient per
//   §17.11's "a record any of whose declared fields is not defaultable".
//
// TWO INDEPENDENT GAPS, both fixed here (TypeResolution_Checks.cpp):
//
//   (a) `typeHasNoDefaultWalk`'s Named-arm null-declaration branch — the
//       SAME leaf `fix #4166`'s review follow-up #3 scoped to the
//       collection family and the FFI-boundary classes by name — never
//       named `ExceptionInfo`/`ReadOnlyStringSpan`, so both fell through
//       to "defer" (= defaultable). This is the ONLY leaf both
//       `ExprTC::defaultIllFormed` (the `default(...)` operator) and
//       `Resolver::argSatisfiesNew`'s Record-field recursion consult, so
//       fixing it here closes the `default(...)` symptom AND the
//       "builtin as a record field under `where T: new()`" symptom in one
//       place.
//
//   (b) `Resolver::argSatisfiesNew` itself: a TOP-LEVEL substitution
//       straight at a null-declaration builtin name (`Holder<T>`
//       instantiated at `ExceptionInfo` directly, not wrapped in a
//       record field) never consulted (a) at all — every one of its
//       dispatch arms requires `arg->declaration` to be non-null, so the
//       function fell straight through to its unconditional "Built-ins,
//       generic params, ...: defer" catch-all regardless of name.
//
//       ⚠ The identically-shaped top-level substitution at `Array<int>`
//       (a name already on the vetted collection list) was ALSO measured
//       silently accepted through this same catch-all — but that is NOT
//       the same defect, and is NOT fixed the same way. §14.2 gives
//       `where T: new()` two INDEPENDENT satisfaction routes: "class
//       types with a public parameterless constructor", or "defaultable
//       value types". A stdlib collection is documented as the FIRST
//       (stdlib §4.4: `Array<T>` is a `public class` with a `public
//       constructor()`) — never evaluated against defaultability — while
//       `ExceptionInfo`/`ReadOnlyStringSpan` are `record`s, for which the
//       SECOND (value-type) route is the only one that could ever apply.
//       A first version of this fix routed EVERY null-declaration Named
//       arg through the shared `typeHasNoDefault` authority (a) already
//       feeds, reasoning it would close both gaps identically — that
//       MEASURABLY regressed the pinned
//       `codegen_generic_new_builtin_collection_2017` test
//       (`Factory.Make<Array<int>>()` must construct a real, usable
//       Array; the wrong route rejected it), caught by this fix's own
//       broad, no-`--area` sweep (LANE_RULES §2/§15) before landing. The
//       shipped fix therefore name-scopes gap (b)'s new check to exactly
//       `ExceptionInfo`/`ReadOnlyStringSpan` — see row (7) below, which is
//       the regression pin for the reverted approach. (`CString`/
//       `WString`/`RawBytes` — also class-shaped, also with no accessible
//       parameterless constructor — were separately measured to ALREADY
//       reject correctly at this exact top-level site, through some
//       pre-existing path unrelated to this arm; not traced further,
//       since it predates this fix and this fix does not disturb it.)
//
// MEASURED on parent `r18-c2` tip `ea66776` (clean tree, `bezelc --version`
// confirmed `v0.279-wave3-eabfa69-873-gea66776`, no `-modified` suffix):
//
//   default(ExceptionInfo)              -> compiled (exit 0), SIGSEGV
//                                           (exit 139) reading `.TypeName`
//   default(ReadOnlyStringSpan)          -> compiled with only the #2491
//                                           degraded-value backstop,
//                                           `.Source.Length` silently
//                                           read as 0 (exit 0)
//   HolderEINF<ExceptionInfo>.Make()      -> compiled with no §14.2
//     under `where T: new()`                diagnostic, SIGSEGV (exit 139)
//
// FAIL-WITHOUT / PASS-WITH, MEASURED on a clean rebuild of parent `ea66776`
// (worktree `/var/tmp/bezel-scratch/r18/iso-4235-parent`, `bezelc --version`
// confirmed `v0.279-wave3-eabfa69-873-gea66776`, no `-modified` suffix):
// **6 of the 9 rows below fail without this fix** — rows (1), (2), (3),
// (4), (8), (9) (every REJECT row) each compile clean (exit 0, no §17.11/
// §14.2 diagnostic) on that parent, and correctly reject with this fix.
// The other 3 rows — (5), (6), (7), the ACCEPT-side positive controls
// (one of them the regression pin for the reverted broader approach) —
// compile clean on BOTH the parent and this fix, identically, which is
// the desired outcome for an over-acceptance fix, not a gap in coverage.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// Asserts a clean, real-location §17.11 rejection of a top-level
// `default(...)` expression — never a runtime crash or a silently
// degraded value (the exact defect class: pre-fix this compiled clean and
// either SIGSEGV'd or read back 0).
void expectDefaultRejected(const std::string& program) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`default(T)` is a compile error"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""17.11"));
}

// Asserts a clean, real-location §14.2 rejection of a `where T: new()`
// substitution — mirrors `arg_satisfies_new_delegating_generic_record_4166_
// test.cpp`'s own `expectWhereNewRejected` helper.
void expectWhereNewRejected(const std::string& program) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "does not satisfy the constraint `where T: new()`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""14.2"));
}

}  // namespace

// ── (1) the issue's first repro, top-level `default(ExceptionInfo)` ──────
BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           default_operator_rejects_exceptioninfo_top_level_spec_17_11) {
    expectDefaultRejected(
        "module M4235a; "
        "class ProgEINF4235a "
        "  public static method Main(): int "
        "    var x := default(ExceptionInfo); "
        "    return x.TypeName.Length; "
        "  end Main; "
        "end ProgEINF4235a;");
}

// ── (2) the issue's second repro, top-level `default(ReadOnlyStringSpan)` ─
BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           default_operator_rejects_readonlystringspan_top_level_spec_17_11) {
    expectDefaultRejected(
        "module M4235b; "
        "class ProgROSS4235b "
        "  public static method Main(): int "
        "    var x := default(ReadOnlyStringSpan); "
        "    return x.Source.Length; "
        "  end Main; "
        "end ProgROSS4235b;");
}

// ── (3) the issue's third repro: TOP-LEVEL `where T: new()` substitution
//        straight at `ExceptionInfo` (gap (b) above — never wrapped in a
//        record field) ────────────────────────────────────────────────────
BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           where_T_new_rejects_exceptioninfo_top_level_substitution_spec_14_2) {
    expectWhereNewRejected(
        "module M4235c; "
        "class HolderEINF4235c<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end HolderEINF4235c; "
        "class ProgEINF4235c "
        "  public static method Main(): int "
        "    var b := HolderEINF4235c<ExceptionInfo>.Make(); "
        "    return b.TypeName.Length; "
        "  end Main; "
        "end ProgEINF4235c;");
}

// ── (4) the twin of (3) for `ReadOnlyStringSpan` — the issue's summary
//        states the `where T: new()` equivalence applies to BOTH names,
//        not only the one its worked example spelled out ─────────────────
BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           where_T_new_rejects_readonlystringspan_top_level_substitution_spec_14_2) {
    expectWhereNewRejected(
        "module M4235d; "
        "class HolderROSS4235d<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end HolderROSS4235d; "
        "class ProgROSS4235d "
        "  public static method Main(): int "
        "    var b := HolderROSS4235d<ReadOnlyStringSpan>.Make(); "
        "    return b.Source.Length; "
        "  end Main; "
        "end ProgROSS4235d;");
}

// ── (5) POSITIVE CONTROL, mandatory (gap (b)'s own near-miss): a VALUE
//        builtin (`Guid`, null-declaration exactly like `ExceptionInfo`,
//        but WITH a real default) substituted at the SAME top-level site
//        must stay ACCEPTED. This is the row that would have failed had
//        gap (b)'s fix routed EVERY null-declaration `Named` arg to
//        rejection instead of consulting the vetted `typeHasNoDefault`
//        authority — mirrors `where_T_new_accepts_record_with_guid_field_
//        spec_14_2` in the #4166 suite, but at the TOP level rather than
//        wrapped in a record field, since gap (b) is a top-level-only
//        code path that suite never exercised. Discards the value and
//        returns a constant (rather than `.ToString().Length`, whose exact
//        formatted length is a detail of `Guid.ToString()` unrelated to
//        this row's point — a zero Guid's dashed form measured 36 here,
//        which would make the assertion look load-bearing on a string
//        format this fix never touches). ─────────────────────────────────
BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           where_T_new_accepts_guid_top_level_substitution_spec_14_2) {
    auto r = runAot(
        "module M4235e; "
        "class HolderGuid4235e<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end HolderGuid4235e; "
        "class ProgGuid4235e "
        "  public static method Main(): int "
        "    var b := HolderGuid4235e<Guid>.Make(); "
        "    _ := b; "
        "    return 0; "
        "  end Main; "
        "end ProgGuid4235e;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── (6) POSITIVE CONTROL: `default(Guid)` at the SAME top level must stay
//        ACCEPTED — cheap insurance that this fix's new
//        `ExceptionInfo`/`ReadOnlyStringSpan` name check in
//        `typeHasNoDefaultWalk` did not widen to catch value builtins too;
//        already pinned by `tests/lit/builtin_closed_ctor_surface_accept_
//        2994.bzl`, restated here beside the rows it protects. Same
//        discard-and-return-0 shape as (5), for the same reason. ────────
BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           default_operator_accepts_guid_top_level_spec_17_11_control) {
    auto r = runAot(
        "module M4235f; "
        "class ProgGuid4235f "
        "  public static method Main(): int "
        "    var x := default(Guid); "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end ProgGuid4235f;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── (7) POSITIVE CONTROL, REQUIRED (this fix's own broad sweep caught a
//        regression here — kept as a permanent regression pin): an
//        IDENTICALLY-SHAPED top-level substitution at `Array<int>` — a
//        collection-family name gap (b)'s FIRST version also (wrongly)
//        routed through `typeHasNoDefault` — must stay ACCEPTED.
//
//        `Array<int>` and `ExceptionInfo` are BOTH null-declaration
//        builtins, and both were measured silently accepted at this exact
//        top-level site on parent `ea66776`. But they are NOT the same
//        defect: §14.2 gives `where T: new()` two independent satisfaction
//        routes — "class types with a public parameterless constructor"
//        or "defaultable value types" — and a stdlib COLLECTION satisfies
//        the FIRST (stdlib §4.4: `Array<T>` is a `public class` with a
//        `public constructor()`; pinned by
//        `tests/codegen/generic_new_builtin_collection_2017_test.cpp`,
//        whose own comment quotes this same §14.2 text), never evaluated
//        against defaultability. `ExceptionInfo`/`ReadOnlyStringSpan` are
//        `record`s — the SECOND (value-type) route is the only one that
//        could ever apply to them. Routing `Array<int>` through
//        `typeHasNoDefault` here answers the wrong one of the two §14.2
//        questions for a class-kind substitution, and this row is exactly
//        the regression that caught it: REJECT here would break
//        `codegen_generic_new_builtin_collection_2017`'s pinned
//        `Factory.Make<Array<int>>()` (Count == 2). ────────────────────
BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           where_T_new_accepts_array_top_level_substitution_no_regression_spec_14_2) {
    auto r = runAot(
        "module M4235g; "
        "class HolderArr4235g<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end HolderArr4235g; "
        "class ProgArr4235g "
        "  public static method Main(): int "
        "    var b := HolderArr4235g<Array<int>>.Make(); "
        "    return b.Count; "
        "  end Main; "
        "end ProgArr4235g;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── (8) FIELD-WRAPPED composition control: `ExceptionInfo` as a RECORD
//        FIELD (not the top-level substitution itself) under
//        `where T: new()` — proves gap (a)'s fix also composes correctly
//        through the PRE-EXISTING Record-arm recursion in
//        `Resolver::argSatisfiesNew` (the same call path `fix #4166`'s own
//        suite exercises for `CString`/`WString`/`RawBytes`), not only the
//        new top-level dispatch added for gap (b). ─────────────────────
BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           where_T_new_rejects_record_with_exceptioninfo_field_spec_14_2) {
    expectWhereNewRejected(
        "module M4235h; "
        "public record BoxEINF4235h(E: ExceptionInfo, N: int); "
        "class Holder4235h<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end Holder4235h; "
        "class Program4235h "
        "  public static method Main(): int "
        "    var b := Holder4235h<BoxEINF4235h>.Make(); "
        "    return b.N; "
        "  end Main; "
        "end Program4235h;");
}

// ── (9) FIELD-WRAPPED composition control, `default(...)` side: an
//        ordinary LOCAL record (a REAL `TypeSymbol`, unlike the builtins
//        above) with an `ExceptionInfo` field must be rejected through the
//        Record-kind arm's OWN recursion (`typeHasNoDefaultWalk`'s
//        `t->declaration->kind == Record` branch), which calls back into
//        the SAME Named-arm leaf gap (a) fixed. Distinguished from rows
//        (1)/(2) by its diagnostic wording: here `rt->declaration` is a
//        real Record TypeSymbol, so the "detail" arm names it explicitly. ─
BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           default_operator_rejects_record_with_exceptioninfo_field_spec_17_11) {
    auto r = runAot(
        "module M4235i; "
        "public record BoxEINF4235i(E: ExceptionInfo, N: int); "
        "class Program4235i "
        "  public static method Main(): int "
        "    var x := default(BoxEINF4235i); "
        "    return x.E.TypeName.Length; "
        "  end Main; "
        "end Program4235i;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`default(T)` is a compile error for a record any of whose "
           "fields is a non-nullable reference type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""17.11"));
}

// ── SEND-BACK on #4235 (review #4245): sixth+ sibling — `Span<T>`/
//    `ReadOnlySpan<T>` (stdlib §4.14) and `ArraySpan<T>`/
//    `ReadOnlyArraySpan<T>` (stdlib §4.15) are the SAME shape as
//    `ExceptionInfo`/`ReadOnlyStringSpan` above: builtin records with an
//    always-non-nullable CLASS-typed declared field (`Buffer:
//    OwningBuffer<T>` for the Span pair, `Source: Array<T>` for the
//    ArraySpan pair — both `OwningBuffer<T>`/`Array<T>` are `public
//    class`es per stdlib §4.14/§4.4), so non-defaultable for EVERY `T`,
//    not merely some — no per-`T` recursion needed, unlike
//    `DictionaryEntry<K,V>` (stdlib §4.8), which review #4245 measured as
//    a HARDER, instantiation-dependent case and which this batch
//    DELIBERATELY leaves open on #4245 (its fields ARE the generic
//    parameters themselves; this null-declaration leaf has no
//    `TypeSymbol` to attach a `RecordSubstEnv` to for that recursion).
//
//    Quoted in full, the two spec facts these 8 rows pin:
//
//      stdlib §4.14: "public record Span<T>(Buffer: OwningBuffer<T>,
//      Start: int, Length: int): IReadOnlySpan<T?>" (and the value-T
//      overload "... IReadOnlySpan<T>") — `Buffer` is unconditionally
//      class-typed.
//
//      stdlib §4.15: "public record ArraySpan<T>(Source: Array<T>, Start:
//      int, Length: int): IReadOnlySpan<T>" — `Source` is unconditionally
//      class-typed. (`ReadOnlySpan<T>`/`ReadOnlyArraySpan<T>` are declared
//      identically apart from the interface's mutability.)
//
//    MEASURED on this file's own PARENT commit `9ba93e3` (this repo's
//    prior state, clean tree, `bezelc` rebuilt directly before measuring,
//    `git status --short` empty) — **8 of 8 rows below fail without this
//    fix**:
//
//      default(Span<int>)/.Length                    -> compile 0, run 0
//      default(ReadOnlySpan<int>)/.Length             -> compile 0, run 0
//      default(ArraySpan<int>)/.Length                -> compile 0, run 0
//      default(ReadOnlyArraySpan<int>)/.Length        -> compile 0, run 0
//      HolderSpan<Span<int>>.Make() where T:new()     -> compile 139
//                                                         (COMPILER SIGSEGV,
//                                                         not a generated-
//                                                         program crash —
//                                                         Emitter::
//                                                         lowerExprInner,
//                                                         LlvmBackend_Exprs.
//                                                         cpp:3830,
//                                                         CreateExtractValue
//                                                         on a bad
//                                                         aggregate)
//      HolderROS<ReadOnlySpan<int>>.Make() where T:new() -> compile 139
//                                                             (COMPILER
//                                                             SIGSEGV,
//                                                             same site)
//      HolderAS<ArraySpan<int>>.Make() where T:new()  -> compile 139
//                                                          (COMPILER
//                                                          SIGSEGV, same
//                                                          site)
//      HolderROAS<ReadOnlyArraySpan<int>>.Make()
//        where T:new()                                -> compile 139
//                                                          (COMPILER
//                                                          SIGSEGV, same
//                                                          site)
//
//    PASS-WITH this fix, same 4 probes rebuilt against the fixed
//    `bezelc`: all 4 `default(...)` rows now reject cleanly with a real-
//    location §17.11 diagnostic (compile exit 1); all 4 `where T: new()`
//    rows now reject cleanly with a real-location §14.2 diagnostic
//    (compile exit 1) — **the compiler SIGSEGV no longer reproduces**:
//    rejecting at the constraint check means `default(...)`/the
//    top-level `new T()` substitution never reaches codegen with an
//    unmonomorphized builtin type parameter, eliminating the crash as a
//    side effect (confirmed by direct measurement on all four names, not
//    assumed from the `Span<int>` case the issue's own repro used).
//
//    Guard rails re-verified UNCHANGED by this addition (§25 — this is
//    four more names on the SAME by-name list, not a re-generalization to
//    a structural predicate): `HolderArrCtl<Array<int>>.Make()` and
//    `HolderGuidCtl<Guid>.Make()` under `where T: new()` still compile
//    clean on the fixed `bezelc` (rows (5)/(7) above; the gtest oracle
//    for `Array<T>`'s own pinned factory shape,
//    `codegen_generic_new_builtin_collection_2017`, and
//    `generic_new_stopwatch_2025`, both re-verified green — see this
//    commit's body for the exact filter run and pass counts).
//    `DictionaryEntry<string,int>` re-measured UNCHANGED on both axes
//    (`default(...)` still compiles 0 / SIGSEGVs at runtime 139;
//    `where T: new()` still compiles 0 with only the #2477/#2491
//    backstop notes) — deliberately untouched, left open on #4245.

// ── (10)–(13): top-level `default(...)` — the four span-family names ────
BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           default_operator_rejects_span_top_level_spec_17_11_stdlib_4_14) {
    expectDefaultRejected(
        "module M4235j; "
        "class ProgSpan4235j "
        "  public static method Main(): int "
        "    var x := default(Span<int>); "
        "    return x.Length; "
        "  end Main; "
        "end ProgSpan4235j;");
}

BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           default_operator_rejects_readonlyspan_top_level_spec_17_11_stdlib_4_14) {
    expectDefaultRejected(
        "module M4235k; "
        "class ProgROS4235k "
        "  public static method Main(): int "
        "    var x := default(ReadOnlySpan<int>); "
        "    return x.Length; "
        "  end Main; "
        "end ProgROS4235k;");
}

BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           default_operator_rejects_arrayspan_top_level_spec_17_11_stdlib_4_15) {
    expectDefaultRejected(
        "module M4235l; "
        "class ProgAS4235l "
        "  public static method Main(): int "
        "    var x := default(ArraySpan<int>); "
        "    return x.Length; "
        "  end Main; "
        "end ProgAS4235l;");
}

BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           default_operator_rejects_readonlyarrayspan_top_level_spec_17_11_stdlib_4_15) {
    expectDefaultRejected(
        "module M4235m; "
        "class ProgROAS4235m "
        "  public static method Main(): int "
        "    var x := default(ReadOnlyArraySpan<int>); "
        "    return x.Length; "
        "  end Main; "
        "end ProgROAS4235m;");
}

// ── (14)–(17): top-level `where T: new()` substitution — the four
//    span-family names. Each of these rows is ALSO the regression test
//    for the #4245 compiler SIGSEGV: pre-fix, the identical program
//    crashed `bezelc` itself (exit 139) rather than failing to compile;
//    `expectWhereNewRejected` asserts a real-location diagnostic instead,
//    which a crash cannot produce (`failedAtCompileTimeWithRealLocation`
//    would be satisfied by the pre-fix crash's own backstop location too
//    — see LANE_RULES §18 — so the MESSAGE-TEXT assertion inside
//    `expectWhereNewRejected` is what actually distinguishes "rejected"
//    from "crashed with a located backstop diagnostic"; the FAIL-WITHOUT
//    measurement above independently confirms the pre-fix side was an
//    exit-139 SIGSEGV, not a located rejection, by direct process exit
//    code, which is stronger than the diagnostic-text check alone). ────
BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           where_T_new_rejects_span_top_level_substitution_spec_14_2) {
    expectWhereNewRejected(
        "module M4235n; "
        "class HolderSpan4235n<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end HolderSpan4235n; "
        "class ProgSpan4235n "
        "  public static method Main(): int "
        "    var b := HolderSpan4235n<Span<int>>.Make(); "
        "    return b.Length; "
        "  end Main; "
        "end ProgSpan4235n;");
}

BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           where_T_new_rejects_readonlyspan_top_level_substitution_spec_14_2) {
    expectWhereNewRejected(
        "module M4235o; "
        "class HolderROS4235o<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end HolderROS4235o; "
        "class ProgROS4235o "
        "  public static method Main(): int "
        "    var b := HolderROS4235o<ReadOnlySpan<int>>.Make(); "
        "    return b.Length; "
        "  end Main; "
        "end ProgROS4235o;");
}

BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           where_T_new_rejects_arrayspan_top_level_substitution_spec_14_2) {
    expectWhereNewRejected(
        "module M4235p; "
        "class HolderAS4235p<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end HolderAS4235p; "
        "class ProgAS4235p "
        "  public static method Main(): int "
        "    var b := HolderAS4235p<ArraySpan<int>>.Make(); "
        "    return b.Length; "
        "  end Main; "
        "end ProgAS4235p;");
}

BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           where_T_new_rejects_readonlyarrayspan_top_level_substitution_spec_14_2) {
    expectWhereNewRejected(
        "module M4235q; "
        "class HolderROAS4235q<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end HolderROAS4235q; "
        "class ProgROAS4235q "
        "  public static method Main(): int "
        "    var b := HolderROAS4235q<ReadOnlyArraySpan<int>>.Make(); "
        "    return b.Length; "
        "  end Main; "
        "end ProgROAS4235q;");
}

// ── fix #4245: DictionaryEntry<K,V> — the SIXTH+ name #4245 found, and the
//    ONE its own review left open ("real design work needing its own
//    mechanism, not a name-list entry") because it is architecturally
//    DIFFERENT from the six names above: stdlib §4.8 declares
//    `public record DictionaryEntry<K, V>(Key: K, Value: V);` — the declared
//    fields ARE the generic parameters themselves (no wrapping), so
//    defaultability is INSTANTIATION-DEPENDENT — `DictionaryEntry<int,int>`
//    IS defaultable; `DictionaryEntry<string,int>` is NOT — unlike Span/
//    ArraySpan/ExceptionInfo/ReadOnlyStringSpan, which are non-defaultable
//    for EVERY instantiation and decided by name alone.
//
//    Fixed in `typeHasNoDefaultWalk`'s null-declaration branch and
//    `Resolver::argSatisfiesNew`'s top-level dispatch (both
//    TypeResolution_Checks.cpp) by recursing into `t->genericArgs`
//    (DictionaryEntry's two "virtual fields") through the SAME
//    `common::resolveRecordFieldSubst`/`RecordSubstEnv` substitution
//    primitive the Record-kind arm already uses for a real record's field
//    walk — not a new mechanism, and not a re-generalization to every
//    null-declaration builtin (§25): this is the one builtin whose field
//    composition is the raw identity of its own generic parameters, so no
//    `TypeSymbol`/`seedRecordSubstEnv` call is needed to know what the
//    "fields" are.
//
// MEASURED on this file's own parent `8ca85ce` (clean tree, `bezelc`
// rebuilt directly before this edit, `bezelc --version` confirmed
// `v0.279-wave3-eabfa69-905-g8ca85ce`, no `-modified` suffix) — **4 of 4
// REJECT rows below fail without this fix**:
//
//   default(DictionaryEntry<string, int>)/.Key.Length      -> compile 0,
//                                                              run 139
//                                                              (SIGSEGV)
//   HolderDE<DictionaryEntry<string,int>>.Make()
//     where T:new(), then .Key.Length                      -> compile 0
//                                                              (only the
//                                                              #2477/#2491
//                                                              backstop
//                                                              notes, no
//                                                              §14.2
//                                                              diagnostic),
//                                                              run 139
//                                                              (SIGSEGV)
//   record BoxDE(D: DictionaryEntry<string,int>, N: int)
//     under where T:new()                                   -> compile 0
//                                                              (silently
//                                                              accepted)
//   record Wrap<T>(D: DictionaryEntry<T, int>), Wrap<string>
//     under where U:new()                                   -> compile 0
//                                                              (silently
//                                                              accepted —
//                                                              the nested-
//                                                              generic /
//                                                              `incoming`-
//                                                              env
//                                                              threading
//                                                              case)
//
// PASS-WITH this fix, same 4 probes rebuilt against the fixed `bezelc`: all
// 4 reject cleanly with a real-location §17.11 or §14.2 diagnostic (compile
// exit 1) — no crash on either axis.
//
// The 4 ACCEPT-side positive controls below (DictionaryEntry<int,int> at
// every one of the same 4 sites) compile clean on BOTH the parent and this
// fix, identically — the desired outcome proving the fix is
// instantiation-dependent, not a blanket rejection of the name. A real
// `Dictionary<K,V>` iteration yielding genuine `DictionaryEntry` values from
// live `for var entry in dict do ... end;` iteration (the actual production
// use this fix must not disturb) is already covered by the pre-existing
// suite — e.g. `dict_entry_iter_548_test.cpp`,
// `dict_entry_key_release_capture_envdtor_4127_test.cpp`,
// `dict_entry_for_var_capture_escaping_release_4161_test.cpp` — swept green
// alongside this file's own filter as part of this fix's own regression
// sweep (see this fix's commit body for the exact pass counts); not
// duplicated here.

BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           default_operator_rejects_dictionary_entry_top_level_spec_17_11_stdlib_4_8) {
    expectDefaultRejected(
        "module M4245a; "
        "class ProgDE4245a "
        "  public static method Main(): int "
        "    var x := default(DictionaryEntry<string, int>); "
        "    return x.Key.Length; "
        "  end Main; "
        "end ProgDE4245a;");
}

BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           default_operator_accepts_dictionary_entry_intint_top_level_spec_17_11_control) {
    auto r = runAot(
        "module M4245b; "
        "class ProgDEii4245b "
        "  public static method Main(): int "
        "    var x := default(DictionaryEntry<int, int>); "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end ProgDEii4245b;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           where_T_new_rejects_dictionary_entry_top_level_substitution_spec_14_2) {
    expectWhereNewRejected(
        "module M4245c; "
        "class HolderDE4245c<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end HolderDE4245c; "
        "class ProgDE4245c "
        "  public static method Main(): int "
        "    var b := HolderDE4245c<DictionaryEntry<string, int>>.Make(); "
        "    return b.Key.Length; "
        "  end Main; "
        "end ProgDE4245c;");
}

BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           where_T_new_accepts_dictionary_entry_intint_top_level_substitution_spec_14_2_control) {
    auto r = runAot(
        "module M4245d; "
        "class HolderDEii4245d<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end HolderDEii4245d; "
        "class ProgDEii4245d "
        "  public static method Main(): int "
        "    var b := HolderDEii4245d<DictionaryEntry<int, int>>.Make(); "
        "    _ := b; "
        "    return 0; "
        "  end Main; "
        "end ProgDEii4245d;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── field-wrapped composition: DictionaryEntry as a RECORD FIELD (not the
//    top-level substitution) under where T: new() — proves the fix composes
//    through the PRE-EXISTING Record-arm recursion in
//    `Resolver::argSatisfiesNew`, not only the new top-level dispatch. ────
BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           where_T_new_rejects_record_with_dictionary_entry_field_spec_14_2) {
    expectWhereNewRejected(
        "module M4245e; "
        "public record BoxDE4245e(D: DictionaryEntry<string, int>, N: int); "
        "class HolderBoxDE4245e<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end HolderBoxDE4245e; "
        "class ProgBoxDE4245e "
        "  public static method Main(): int "
        "    var b := HolderBoxDE4245e<BoxDE4245e>.Make(); "
        "    return b.N; "
        "  end Main; "
        "end ProgBoxDE4245e;");
}

BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           where_T_new_accepts_record_with_dictionary_entry_intint_field_spec_14_2_control) {
    auto r = runAot(
        "module M4245f; "
        "public record BoxDE4245f(D: DictionaryEntry<int, int>, N: int); "
        "class HolderBoxDE4245f<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end HolderBoxDE4245f; "
        "class ProgBoxDE4245f "
        "  public static method Main(): int "
        "    var b := HolderBoxDE4245f<BoxDE4245f>.Make(); "
        "    return b.N; "
        "  end Main; "
        "end ProgBoxDE4245f;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── nested-generic composition, the `incoming`-env threading case: a
//    generic record `Wrap<T>(D: DictionaryEntry<T, int>)` instantiated at
//    `Wrap<string>` under `where U: new()`. Before this recursion sees
//    genericArgs[0] (a BARE reference to `T`, the enclosing record's own
//    generic parameter) it must resolve `T` through the incoming
//    `RecordSubstEnv` the Record-arm seeded from `Wrap<string>`'s own
//    `genericArgs` — exactly the composition
//    `common::resolveRecordFieldSubst`/`RecordSubstEnv` exist for. This is
//    the row that would have failed had the fix's recursion passed
//    `incoming=nullptr` instead of threading it through. ──────────────────
BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           where_T_new_rejects_nested_generic_dictionary_entry_field_spec_14_2) {
    expectWhereNewRejected(
        "module M4245g; "
        "public record Wrap4245g<T>(D: DictionaryEntry<T, int>); "
        "class HolderWrap4245g<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end HolderWrap4245g; "
        "class ProgWrap4245g "
        "  public static method Main(): int "
        "    var w := HolderWrap4245g<Wrap4245g<string>>.Make(); "
        "    _ := w; "
        "    return 0; "
        "  end Main; "
        "end ProgWrap4245g;");
}

BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           where_T_new_accepts_nested_generic_dictionary_entry_intint_field_spec_14_2_control) {
    auto r = runAot(
        "module M4245h; "
        "public record Wrap4245h<T>(D: DictionaryEntry<T, int>); "
        "class HolderWrap4245h<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end HolderWrap4245h; "
        "class ProgWrap4245h "
        "  public static method Main(): int "
        "    var w := HolderWrap4245h<Wrap4245h<int>>.Make(); "
        "    _ := w; "
        "    return 0; "
        "  end Main; "
        "end ProgWrap4245h;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── default(...) side of the field-wrapped and nested-generic composition
//    rows above — proves the SAME recursion also composes through
//    `typeHasNoDefaultWalk`'s pre-existing Record-kind arm (the
//    `default(...)` operator path, not just the `where T: new()` path). ──
BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           default_operator_rejects_record_with_dictionary_entry_field_spec_17_11) {
    auto r = runAot(
        "module M4245i; "
        "public record BoxDE4245i(D: DictionaryEntry<string, int>, N: int); "
        "class ProgBoxDE4245i "
        "  public static method Main(): int "
        "    var x := default(BoxDE4245i); "
        "    return x.N; "
        "  end Main; "
        "end ProgBoxDE4245i;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`default(T)` is a compile error for a record any of whose "
           "fields is a non-nullable reference type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""17.11"));
}

BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           default_operator_rejects_nested_generic_dictionary_entry_field_spec_17_11) {
    auto r = runAot(
        "module M4245j; "
        "public record Wrap4245j<T>(D: DictionaryEntry<T, int>); "
        "class ProgWrap4245j "
        "  public static method Main(): int "
        "    var x := default(Wrap4245j<string>); "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end ProgWrap4245j;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`default(T)` is a compile error for a record any of whose "
           "fields is a non-nullable reference type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""17.11"));
}

// ── fix #4294: IGrouping<TKey,T> — the SEVENTH name on this file's null-
//    declaration-builtin list, found while re-measuring #4245's own table
//    (issue #4294's own filing). `IGrouping<TKey, T>` (stdlib §5: `public
//    interface IGrouping<TKey, T>: IEnumerable<T> readonly property Key:
//    TKey; end IGrouping;`) is a null-declaration builtin — arity-2, no
//    user `TypeSymbol` (TypeResolution_Names.cpp's arity table, alongside
//    DictionaryEntry/DrainedEntry) — but architecturally SIMPLER than
//    DictionaryEntry: it is an INTERFACE, unconditionally, regardless of
//    `TKey`/`T`, so — like the six-name Span/ExceptionInfo block above —
//    a flat by-name check decides it with no per-argument recursion.
//
//    Quoted in full, the two spec facts this section pins:
//
//      §17.11's "defaultable" glossary entry: "Not defaultable: non-
//      nullable reference types...". Every interface is a non-nullable
//      reference type (no `?`), so `default(IGrouping<...>)` is ill-formed
//      exactly as `default(SomeUserInterface)` already was for a REAL
//      (TypeSymbol-backed) interface — see this file's
//      `typeHasNoDefaultWalk`'s `k == Class || k == Interface -> return
//      !isValueBuiltin` arm, which already covers that case; IGrouping
//      just has no `TypeSymbol` to reach it through.
//
//      §14.2: "Interface substitutions never satisfy `new()` — no
//      constructor exists." The TypeSymbol-backed `d->kind ==
//      ast::TypeDeclKind::Interface` arm in `Resolver::argSatisfiesNew`
//      already applies this unconditionally for a real interface;
//      IGrouping needs the identical, unconditional conclusion.
//
//    Fixed in `TypeResolution_Checks.cpp` as TWO NEW, SELF-CONTAINED
//    branches (not folded into the pre-existing six-name Span/
//    ExceptionInfo block in either function) — see this fix's commit body
//    for why: that block's gate is validated to answer a DIFFERENT
//    question ("is this builtin RECORD's fixed field composition always
//    non-defaultable"), and folding IGrouping into it would reach the
//    right answer only by two independently-true facts agreeing, not by
//    reusing a check for the question it was built to answer (§25).
//
// MEASURED on this file's own parent `f3d3a4d` (clean tree, `bezelc`
// rebuilt directly before this edit, `bezelc --version` confirmed
// `v0.279-wave3-eabfa69-909-g3986876`, no `-modified` suffix) — **6 of 6
// REJECT rows below fail without this fix**:
//
//   default(IGrouping<string, int>)                        -> compile 0,
//                                                              run 0
//   HolderIG<IGrouping<string,int>>.Make() where T:new()   -> compile 0
//                                                              (only the
//                                                              #2477/#2491
//                                                              backstop
//                                                              notes, no
//                                                              §14.2
//                                                              diagnostic),
//                                                              run 0
//   record BoxIG(G: IGrouping<string,int>, N: int)
//     under default(...)                                    -> compile 0
//   record BoxIG2(G: IGrouping<string,int>, N: int)
//     under where T:new()                                   -> compile 0
//   record Wrap<T>(G: IGrouping<T,int>), Wrap<string>
//     under default(...)                                     -> compile 0
//   record Wrap<T>(G: IGrouping<T,int>), Wrap<string>
//     under where U:new()                                    -> compile 0
//
// PASS-WITH this fix, same 6 probes rebuilt against the fixed `bezelc`:
// all 6 reject cleanly with a real-location §17.11 or §14.2 diagnostic
// (compile exit 1) — verified by direct manual probe before writing these
// gtest rows, then re-verified mechanically via this file's own filter
// (see this fix's commit body for the exact FAIL-WITHOUT/PASS-WITH gtest
// counts and pass rates).
//
// UNLIKE `DictionaryEntry<K,V>`, there is no defaultable IGrouping
// instantiation to use as a per-row ACCEPT-side positive control — an
// interface is non-defaultable regardless of `TKey`/`T`, so there is no
// "IGrouping<int,int>" analogue of DictionaryEntry's int/int rows. The
// accept-side control this section carries instead is the actual
// production use this fix must not disturb: a REAL `IGrouping` obtained
// from `IEnumerable<T>.GroupBy` (stdlib §5) used legitimately — reading
// `.Key` and iterating its elements — which never reaches `default(...)`
// or `where T: new()` at all and must keep compiling and computing the
// right value. The existing accept-side controls for the OTHER names this
// file's `argSatisfiesNew`/`typeHasNoDefaultWalk` branches touch (`Guid`
// at rows (5)/(6), `Array<int>` at row (7), `DictionaryEntry<int,int>` at
// the #4245 rows above) are re-swept as part of this fix's own filter run
// rather than duplicated here, since this fix adds two brand-new,
// self-contained branches that do not modify their existing conditions.

BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           default_operator_rejects_igrouping_top_level_spec_17_11_stdlib_5) {
    expectDefaultRejected(
        "module M4294a; "
        "class ProgIG4294a "
        "  public static method Main(): int "
        "    var x := default(IGrouping<string, int>); "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end ProgIG4294a;");
}

BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           where_T_new_rejects_igrouping_top_level_substitution_spec_14_2) {
    expectWhereNewRejected(
        "module M4294b; "
        "class HolderIG4294b<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end HolderIG4294b; "
        "class ProgIG4294b "
        "  public static method Main(): int "
        "    var b := HolderIG4294b<IGrouping<string, int>>.Make(); "
        "    _ := b; "
        "    return 0; "
        "  end Main; "
        "end ProgIG4294b;");
}

// ── field-wrapped composition: IGrouping as a RECORD FIELD (not the
//    top-level substitution) under `default(...)` and `where T: new()` —
//    proves the fix composes through the PRE-EXISTING Record-arm
//    recursion (`typeHasNoDefaultWalk`'s `Record`-kind arm /
//    `argSatisfiesNew`'s `Record`-kind arm's `recordStorageMembers` walk),
//    not only the new top-level dispatch. ─────────────────────────────
BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           default_operator_rejects_record_with_igrouping_field_spec_17_11) {
    auto r = runAot(
        "module M4294c; "
        "public record BoxIG4294c(G: IGrouping<string, int>, N: int); "
        "class ProgBoxIG4294c "
        "  public static method Main(): int "
        "    var x := default(BoxIG4294c); "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end ProgBoxIG4294c;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`default(T)` is a compile error for a record any of whose "
           "fields is a non-nullable reference type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""17.11"));
}

BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           where_T_new_rejects_record_with_igrouping_field_spec_14_2) {
    expectWhereNewRejected(
        "module M4294d; "
        "public record BoxIG4294d(G: IGrouping<string, int>, N: int); "
        "class HolderBoxIG4294d<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end HolderBoxIG4294d; "
        "class ProgBoxIG4294d "
        "  public static method Main(): int "
        "    var b := HolderBoxIG4294d<BoxIG4294d>.Make(); "
        "    return b.N; "
        "  end Main; "
        "end ProgBoxIG4294d;");
}

// ── nested-generic composition, the `incoming`-env threading case: a
//    generic record `Wrap<T>(G: IGrouping<T, int>)` instantiated at
//    `Wrap<string>` — mirrors the #4245 DictionaryEntry nested-generic
//    rows, proving `IGrouping`'s new branches are reached identically
//    whether the outer generic argument is a literal name or a bare,
//    not-yet-substituted outer type parameter. ─────────────────────────
BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           default_operator_rejects_nested_generic_igrouping_field_spec_17_11) {
    auto r = runAot(
        "module M4294e; "
        "public record Wrap4294e<T>(G: IGrouping<T, int>); "
        "class ProgWrap4294e "
        "  public static method Main(): int "
        "    var x := default(Wrap4294e<string>); "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end ProgWrap4294e;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`default(T)` is a compile error for a record any of whose "
           "fields is a non-nullable reference type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""17.11"));
}

BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           where_T_new_rejects_nested_generic_igrouping_field_spec_14_2) {
    expectWhereNewRejected(
        "module M4294f; "
        "public record Wrap4294f<T>(G: IGrouping<T, int>); "
        "class HolderWrap4294f<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end HolderWrap4294f; "
        "class ProgWrap4294f "
        "  public static method Main(): int "
        "    var w := HolderWrap4294f<Wrap4294f<string>>.Make(); "
        "    _ := w; "
        "    return 0; "
        "  end Main; "
        "end ProgWrap4294f;");
}

// ── ACCEPT-SIDE CONTROL, REQUIRED (LANE_RULES §15): a REAL `IGrouping`
//    produced by `IEnumerable<T>.GroupBy` (stdlib §5) and used
//    legitimately — reading `.Key`, iterating its elements — must keep
//    compiling AND computing the right value. This never reaches
//    `default(...)` or `where T: new()` (GroupBy always returns a live,
//    materialized grouping, never a zero-initialized one), so it is
//    orthogonal to both new branches, but it is the actual production use
//    those branches must not disturb.
//
//    xs = [1,2,3,4] keyed `x mod 2` -> two groups: key 1 (odd) = {1,3},
//    key 0 (even) = {2,4}. total = (1*100 + 1+3) + (0*100 + 2+4) = 104 +
//    6 = 110 — the identical shape and expected value as the pre-existing
//    `kInnerForThenKey` case in
//    `tests/codegen/igrouping_value_model_755_test.cpp` (issue #755),
//    confirming this row exercises the same, already-correct GroupBy
//    value model this fix's sema-only change does not touch. ──────────
BEZEL_TEST(default_and_new_reject_builtin_record_string_fields_4235,
           groupby_real_usage_accept_igrouping_spec_5_control) {
    auto r = runAot(
        "module M4294g; "
        "import System.Collections.Linq; "
        "class ProgAccept4294g "
        "  public static method Main(): int "
        "    var xs := new Array<int>(); "
        "    xs.Add(1); xs.Add(2); xs.Add(3); xs.Add(4); "
        "    var total := 0; "
        "    for var g in xs.GroupBy(lambda(x: int) => x mod 2) do "
        "      total := total + g.Key * 100; "
        "      for var el in g do total := total + el; end; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end ProgAccept4294g;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 110);
}
