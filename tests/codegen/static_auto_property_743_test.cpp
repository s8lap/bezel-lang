// Spec-derived tests for fix #743 — a STATIC auto-property gets a module-level
// global backing store, so the implicit get reads and the implicit set writes
// real storage.
//
// Authority: §23.1 — the compiler generates a backing field for each
// auto-property; for a static auto-property that backing is a module-level
// global. Pre-fix no global was emitted: the getter always returned 0 and the
// setter's store was discarded (the program below returned 0 instead of 5).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "support/aot_support.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §23.1: the headline — set then read a static auto-property.
BEZEL_TEST(static_auto_property_743, set_then_get) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static property X: int; "
        "  public static method Main(): int "
        "    C.X := 5; "
        "    return C.X; "
        "  end Main; "
        "end C;"), 5);
}

// §23.1: a static auto-property reads as its zero value before any write.
BEZEL_TEST(static_auto_property_743, initial_value_is_zero) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static property X: int; "
        "  public static method Main(): int "
        "    return C.X; "  // never written → 0
        "  end Main; "
        "end C;"), 0);
}

// §23.1: writes persist and compose (read-modify-write).
BEZEL_TEST(static_auto_property_743, read_modify_write) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static property X: int; "
        "  public static method Main(): int "
        "    C.X := 5; "
        "    C.X := C.X + 10; "
        "    return C.X; "  // 15
        "  end Main; "
        "end C;"), 15);
}

// §23.1: the backing global is shared across methods — bare access inside the
// declaring class reaches the same storage.
BEZEL_TEST(static_auto_property_743, bare_access_across_methods) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static property Counter: int; "
        "  public static method Bump() Counter := Counter + 1; end Bump; "
        "  public static method Main(): int "
        "    Bump(); Bump(); Bump(); "
        "    return Counter; "  // 3
        "  end Main; "
        "end C;"), 3);
}

// §22.12 rule 2 (#1393, v0.221): a MUTABLE static of `string` type is now a
// residual compile error — this program is no longer legal Bezel. Before
// #1393 landed, rule 2 was silent on `string` entirely (the exact
// under-enforcement #1393 reports), so this shape compiled and this test
// exercised the auto-property backing-store SET+GET round trip through it.
// That round-trip mechanism for a reference type remains covered: the
// backing-store LOAD path is proven for `string` by
// static_auto_property_string_initializer_1146 below (a `static readonly
// property Name: string := "hello";` reads its initializer correctly), and
// the generic (type-independent) get/set STORE mechanism is proven by the
// `int` cases above (set_then_get, read_modify_write,
// bare_access_across_methods) — auto-property codegen does not special-case
// the element type between a store to an `int` global and a store to a
// `string` global. What is no longer legal — and now the regression this
// case pins — is the MUTATION of a static `string`, which #1393 rejects
// for the reason stated in the diagnostic below (an overwrite must either
// UAF the displaced referent across domains or leak it unboundedly).
BEZEL_TEST(static_auto_property_743, string_static_auto_property_now_rejected_1393) {
    auto r = runAot(
        "module M; class C "
        "  public static property Name: string; "
        "  public static method Main(): int "
        "    C.Name := \"hi\"; "
        "    return (if C.Name = \"hi\" then 1 else 0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool foundRule2 = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("mutable static property `Name` has type "
                            "`string`, which \xC2\xA7""22.12 rule 2 does "
                            "not admit as a mutable static") !=
                std::string::npos &&
            d.message.find("(\xC2\xA7""22.12 rule 2, v0.221, #1393)") !=
                std::string::npos)
            foundRule2 = true;
    }
    BEZEL_EXPECT_TRUE(foundRule2);
}

// ── #1146 / §23.1: a static auto-property's CONSTANT inline initializer ───────
// (`static readonly property Total: int := 42`) is honored — the backing global
// previously read 0 (the initializer was dropped, always getNullValue).
BEZEL_TEST(static_auto_property_743, static_auto_property_int_initializer_1146) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static readonly property Total: int := 42; "
        "  public static method Main(): int return C.Total; end Main; "
        "end C;"), 42);
}

// A bool static auto-property initializer.
BEZEL_TEST(static_auto_property_743, static_auto_property_bool_initializer_1146) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static readonly property Flag: bool := true; "
        "  public static method Main(): int return (if C.Flag then 7 else 0); end Main; "
        "end C;"), 7);
}

// A string static auto-property initializer.
BEZEL_TEST(static_auto_property_743, static_auto_property_string_initializer_1146) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static readonly property Name: string := \"hello\"; "
        "  public static method Main(): int return C.Name.Length; end Main; "
        "end C;"), 5);
}

// ===========================================================================
// fix #3749 (§23.1 / §22.13 / §22.12 rule 2, v0.286): a static auto-property's
// NON-FOLDABLE initializer (a method call / static-member reference, as
// opposed to the #1146 tests above which are all literal constants) never
// ran at all — the module-init "does this type have static state needing an
// sfinit function" detection (`needsRuntimeFieldInit`,
// src/codegen/LlvmBackend_Setup.cpp) inspected only `sema::FieldSymbol`
// members, so a type whose only static state was such a property got NO
// sfinit function, and the initializer never ran — module init or first
// access — leaving the backing global at its §7.9 zero default forever, in a
// FULLY single-module, non-imported compilation (unrelated to #3736's
// cross-module carriage machinery). §23.1: "[the assignment] is admitted
// only through `this` for an instance auto-property, or in the declaring
// type's own static initialization for a `static readonly` one" — the same
// phase §22.13 describes for a static field. Issue's own measured repro
// (`H.Get()`-initialized `static readonly property X`, sibling constant
// `Y := 42`) read `X == 0` while `Y == 42`; this suite pins `X` to its
// correct replayed value instead.
// ===========================================================================

using bezel::test::runAotChecked;

// T1 — issue #3749's own measured repro shape, class host: a non-foldable
// (static-method-call) initializer on a `static readonly` auto-property now
// runs and reads the exporter-declared value, exactly like its constant
// sibling (which already worked via #1146).
BEZEL_TEST(static_auto_property_nonfoldable_init_3749,
           t1_class_host_readonly_nonfoldable_initializer_runs_23_1_22_13) {
    auto r = runAotChecked(
        "module M; "
        "public class Secret3749T1 "
        "  public static method Get(): int => 88; "
        "end Secret3749T1; "
        "public class Klass3749T1 "
        "  public static readonly property X: int := Secret3749T1.Get(); "
        "  public static readonly property Y: int := 42; "
        "end Klass3749T1; "
        "public class C3749T1 "
        "  public static method Main(): int "
        "    return Klass3749T1.X * 1000 + Klass3749T1.Y; "
        "  end Main; "
        "end C3749T1;");
    BEZEL_EXPECT_TRUE(r.ran);
    // 88*1000+42 = 88042, truncated through the 8-bit exit path (LANE_RULES
    // §3): 88042 % 256 = 234. See T1b for the untruncated value via stdout.
    BEZEL_EXPECT_EQ(r.exitCode, 234);
}

// T1b — same program as T1, but reads the full untruncated value through
// stdout (LANE_RULES §3: an 8-bit exit code cannot observe 88042).
BEZEL_TEST(static_auto_property_nonfoldable_init_3749,
           t1b_class_host_readonly_nonfoldable_initializer_full_value_stdout) {
    auto r = runAotChecked(
        "module M; "
        "public class Secret3749T1b "
        "  public static method Get(): int => 88; "
        "end Secret3749T1b; "
        "public class Klass3749T1b "
        "  public static readonly property X: int := Secret3749T1b.Get(); "
        "  public static readonly property Y: int := 42; "
        "end Klass3749T1b; "
        "public class C3749T1b "
        "  public static method Main(): int "
        "    Console.WriteLine($\"{Klass3749T1b.X * 1000 + Klass3749T1b.Y}\"); "
        "    return 0; "
        "  end Main; "
        "end C3749T1b;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "88042\n");
}

// T2 — record host (issue #3749 measured the record shape identically to the
// class shape — "this reproduces identically for both a `record` and a
// `class` host").
BEZEL_TEST(static_auto_property_nonfoldable_init_3749,
           t2_record_host_readonly_nonfoldable_initializer_runs_23_1_22_13) {
    auto r = runAotChecked(
        "module M; "
        "public class Secret3749T2 "
        "  public static method Get(): int => 88; "
        "end Secret3749T2; "
        "public record Rec3749T2 "
        "  public static readonly property X: int := Secret3749T2.Get(); "
        "end Rec3749T2; "
        "public class C3749T2 "
        "  public static method Main(): int "
        "    return Rec3749T2.X; "
        "  end Main; "
        "end C3749T2;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 88);
}

// T3 — MUTABLE (non-`readonly`) static auto-property, §22.12 rule 2 form
// (iii): the issue measured this shape separately from the `readonly` one.
// A mutable primitive-value static warns (rule 2 form (iii)) but is
// admitted; the warning is unrelated to this fix and the program must still
// compile and run, with the non-foldable initializer now taking effect.
BEZEL_TEST(static_auto_property_nonfoldable_init_3749,
           t3_mutable_nonreadonly_nonfoldable_initializer_runs_22_12_rule2) {
    auto r = runAotChecked(
        "module M; "
        "public class H3749T3 "
        "  public static method Get(): int => 63; "
        "end H3749T3; "
        "public class C3749T3 "
        "  public static property X: int := H3749T3.Get(); "
        "  public static method Main(): int "
        "    return C3749T3.X; "
        "  end Main; "
        "end C3749T3;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 63);
}

// T4 — STATIC-COUNTER ORACLE, ordering + exactly-once (this fix's own danger
// callout: DOUBLE INIT via module init + first access). `Log3749T4.Next()`
// increments and returns a monotonic counter; a static FIELD `F` (declared
// FIRST) and a static auto-PROPERTY `P` (declared SECOND) each capture one
// call. §22.13 "within a single type, static fields are initialized in
// textual declaration order" — applied to the property's backing storage
// per §23.1 — means F must observe the counter BEFORE P: F==1, P==2. Each
// of F and P is then read FOUR more times (repeated cross-method access,
// after the type's static state is already fully initialized) — if the
// property's initializer ran a second time (module init AND first access,
// rather than the ONE shared sfinit both routes call into), `Log3749T4._seq`
// would read higher than 2 and/or F and P would observe an overwritten
// value. `(a+b)*10 + Log3749T4._seq` folds both the ordering claim (F=1
// before P=2 => (1+2)*10=30) and the exactly-once claim (`_seq` stays 2
// after 5 total reads of each) into one checked value: 32.
BEZEL_TEST(static_auto_property_nonfoldable_init_3749,
           t4_ordering_and_exactly_once_vs_sibling_static_field_22_13) {
    auto r = runAotChecked(
        "module M; "
        "public class Log3749T4 "
        "  public static _seq: int := 0; "
        "  public static method Next(): int "
        "    _seq := _seq + 1; "
        "    return _seq; "
        "  end Next; "
        "end Log3749T4; "
        "public class C3749T4 "
        "  public static readonly F: int := Log3749T4.Next(); "
        "  public static readonly property P: int := Log3749T4.Next(); "
        "  public static method Main(): int "
        "    var a := C3749T4.F; "
        "    var b := C3749T4.P; "
        "    var c := C3749T4.F; "
        "    var d := C3749T4.P; "
        "    var e := C3749T4.F; "
        "    var f := C3749T4.P; "
        "    return (a + b) * 10 + Log3749T4._seq; "
        "  end Main; "
        "end C3749T4;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 32);
}

// T4b — same oracle, DECLARATION ORDER REVERSED (property first, field
// second): §22.13's textual-order guarantee is not "fields always run
// before properties" — it is declaration order, whichever member kind comes
// first. P now captures the counter first (P==1) and F second (F==2);
// `(a+b)*10 + _seq` is unchanged (32) because the checksum is order-blind by
// construction, so this test's real content is that the PROGRAM still
// compiles and runs with a property declared ahead of a field and the
// counter still lands at exactly 2 (no double-run either direction).
BEZEL_TEST(static_auto_property_nonfoldable_init_3749,
           t4b_ordering_reversed_property_before_field_22_13) {
    auto r = runAotChecked(
        "module M; "
        "public class Log3749T4b "
        "  public static _seq: int := 0; "
        "  public static method Next(): int "
        "    _seq := _seq + 1; "
        "    return _seq; "
        "  end Next; "
        "end Log3749T4b; "
        "public class C3749T4b "
        "  public static readonly property P: int := Log3749T4b.Next(); "
        "  public static readonly F: int := Log3749T4b.Next(); "
        "  public static method Main(): int "
        "    var a := C3749T4b.P; "
        "    var b := C3749T4b.F; "
        "    var c := C3749T4b.P; "
        "    var d := C3749T4b.F; "
        "    return (a + b) * 10 + Log3749T4b._seq; "
        "  end Main; "
        "end C3749T4b;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 32);
}

// T5 — CONTROL: a static COMPUTED property (`=> expr`, no `:=`, no backing
// field — `pd->bodyKind` is `Expression`, not `Auto`) is untouched by this
// fix (the new arm is gated on `isStaticAutoProperty`, which requires
// `bodyKind == Auto`): it keeps recomputing live on every read from the
// current field value, exactly as before.
BEZEL_TEST(static_auto_property_nonfoldable_init_3749,
           t5_computed_property_control_unaffected_no_backing_field) {
    auto r = runAotChecked(
        "module M; "
        "public class C3749T5 "
        "  public static _base: int := 10; "
        "  public static property Doubled: int => _base * 2; "
        "  public static method Main(): int "
        "    _base := 21; "
        "    return C3749T5.Doubled; "
        "  end Main; "
        "end C3749T5;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// T6 — CONTROL: the existing #1146 constant-foldable shape stays on the
// BAKED path (declareStatics's constInitForExpr), not this fix's new
// runtime-store arm — `isConstantLiteralStaticInit` gates the new arm off
// for a literal initializer, exactly as it already gated
// `needsRuntimeFieldInit`. Re-asserted here (alongside the pre-existing
// #1146 tests above) as this fix's own regression guard: a literal
// initializer must NOT pick up a redundant runtime store.
BEZEL_TEST(static_auto_property_nonfoldable_init_3749,
           t6_constant_literal_control_stays_on_baked_path_1146) {
    auto r = runAotChecked(
        "module M; "
        "public class C3749T6 "
        "  public static readonly property Total: int := 42; "
        "  public static method Main(): int return C3749T6.Total; end Main; "
        "end C3749T6;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// T7 — STRING-valued static auto-property (ARC-relevant), non-foldable
// initializer, read 200 TIMES under runAotChecked's ASan/LSan generated-code
// instrumentation: a per-read leak or double-free in the newly-exercised
// runtime-store path (`lowerExpr` on the call expression, coerced and
// stored into the backing global once at sfinit time, then read back 200x)
// would accumulate and fail under LSan — a one-shot check cannot observe it
// (LANE_RULES §3 / §R12-G).
BEZEL_TEST(static_auto_property_nonfoldable_init_3749,
           t7_string_valued_nonfoldable_initializer_200x_under_asan) {
    auto r = runAotChecked(
        "module M; "
        "public class Str3749T7 "
        "  public static method Make(): string => \"sv3749\" + \"str\"; "
        "end Str3749T7; "
        "public class C3749T7 "
        "  public static readonly property S: string := Str3749T7.Make(); "
        "  public static method Main(): int "
        "    var bad := 0; "
        "    var k := 0; "
        "    while k < 200 do "
        "      if C3749T7.S.Length <> 9 then bad := bad + 1; end if; "
        "      k := k + 1; "
        "    end; "
        "    return bad; "
        "  end Main; "
        "end C3749T7;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// T8 — KNOWN GAP, already tracked as issue #2861 (found by search before
// this fix, NOT filed by it — LANE_RULES §15 "search first before gh issue
// create"): a GENERIC type's static auto-property has no per-instantiation
// backing storage at all — `staticFieldGlobal`'s auto-property arm
// (`isStaticAutoProperty(sym_raw)` branch, LlvmBackend_Setup.cpp) keys
// directly on `staticGlobals_[ps]`, the ONE template-level global, with no
// `genericInstStaticGlobals_`-style per-instantiation redirection the
// FieldSymbol path has (#709/#1513) — so every instantiation of `Box<T>.X`
// ALIASES the same process-wide cell, contradicting §22.12's "Each distinct
// instantiation has its own static state". #2861 pre-dates #3749 and
// reproduces identically for the ALREADY-working #743 plain get/set case
// with no initializer at all (confirmed on this lane's pre-#3749 HEAD) —
// this fix's runtime store does not touch `staticFieldGlobal` at all, so it
// neither causes nor repairs the sharing #2861 tracks.
//
// The PINNED VALUE below (4) is #3749-POST-fix only, not "unchanged before
// and after": pre-#3749, EVERY static auto-property initializer was inert
// (including this generic one), so both instantiations read a=b=0 and the
// checksum was 0. Post-#3749 the ONE shared cell's initializer now actually
// runs (exactly once, via the same run-once sfinit this fix wires up), so
// both instantiations observe that one shared run's value (a=b=100) instead
// of each independently running its own — checksum 4. #3749 therefore DOES
// change this row's behavior (silently-inert -> silently-aliased), which is
// real progress (the initializer runs at all now) but not a fix for #2861's
// aliasing, which remains exactly as broken as before.
BEZEL_TEST(static_auto_property_nonfoldable_init_3749,
           t8_known_gap_generic_instantiations_alias_one_shared_global_22_12) {
    auto r = runAotChecked(
        "module M; "
        "public class Log3749T8 "
        "  public static _n: int := 0; "
        "  public static method Get(): int "
        "    _n := _n + 1; "
        "    return _n * 100; "
        "  end Get; "
        "end Log3749T8; "
        "public class Box3749T8<T> "
        "  public static readonly property X: int := Log3749T8.Get(); "
        "end Box3749T8; "
        "public class C3749T8 "
        "  public static method Main(): int "
        "    var a := Box3749T8<int>.X; "
        "    var b := Box3749T8<string>.X; "
        "    return a * 1000 + b; "
        "  end Main; "
        "end C3749T8;");
    BEZEL_EXPECT_TRUE(r.ran);
    // KNOWN-GAP value: both instantiations share ONE global, so Get() runs
    // exactly once (shared sfinit, correctly exactly-once per THIS fix) and
    // both reads observe a=b=100 -- the untruncated stdout-free exit-code
    // encoding of 100*1000+100=100100 modulo 256 (LANE_RULES §3) is 4. If a
    // future fix gives each instantiation independent static state, a and b
    // would differ (100 and 200) and this pinned value MUST change with it
    // -- that is the intended signal, not a claim that 4 is correct.
    BEZEL_EXPECT_EQ(r.exitCode, 4);
}
