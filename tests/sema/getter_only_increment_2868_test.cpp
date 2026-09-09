// Regression tests for #2868 — `++`/`--` on a read-only property OR indexer was
// SILENTLY DISCARDED, while the `:=` and `+=` spellings of the same write were
// correctly rejected.
//
// WHAT THE DOCUMENTS REQUIRE. §17.6 (v0.240), verbatim:
//
//     "The operand must be an assignable lvalue (a local variable, a writable
//      field, an indexer slot, or a property with a setter — the same storage
//      categories §22.3's `ref`/`out` arguments reuse). Read-only locations
//      cannot be incremented."
//
// §23.3 (v0.240) supplies the shape rule the operand must satisfy — "A read-only
// property in block form omits `set`" — and §23.4 extends it to the bracketed
// spelling: an indexer is "a property whose name is replaced by `[parameters]`".
// §6.6 makes the identification explicit for the compound family: "§17.6's
// indexer desugaring is the indexer instance of this rule."
//
// MEASURED before the fix, on `7c466ee`, with a getter-only property `P` and a
// getter-only indexer on the same class:
//
//     b.P := 99;   -> rejected  (§23.3)
//     b[0] := 99;  -> rejected  (§23.3, since #2851)
//     b[0] += 1;   -> rejected  (§23.3, since #2851)
//     b.P++;       -> compiled clean, RAN, read back 7 — write DISCARDED
//     b[0]++;      -> compiled clean, RAN, read back 7 — write DISCARDED
//
// THE CAUSE. `visitStmt`'s `ast::IncrementStmt` arm was a third hand-maintained
// mirror of the assignment rules; it never reached the §23.3 assignment-target
// check at all. That mirror had already been found missing a check once (#2309,
// a positional record's auto-generated property), whose own comment states the
// general problem: "the IncrementStmt path never reaches either".
//
// THE FIX consolidates rather than adding a fourth copy: the §23.3/§23.1/§13.6
// property-or-indexer write-target check moved out of the AssignmentStmt arm
// into `ExprTC::checkPropertyWriteTarget`, which BOTH statement forms now call.
// That mirrors what #2848 did on the codegen side (it deleted codegen's mirror
// and routed `a[i]++` through the compound path) on §6.6's own authority.
//
// The `incOp` argument selects the increment wording and adds the §17.6
// citation; every assignment-path diagnostic is byte-identical to what it was
// before the extraction, which the whole lit diagnostic track pins.
//
// Every rejection below asserts on diagnostic TEXT, never the coarse "did not
// run" signal: runAot's WIFSIGNALED branch synthesizes an Error for a runtime
// SIGSEGV too, so a bare `!ran` cannot tell a genuine rejection from a crash.
// Every accept-control asserts a CORRECT VALUE, not a clean compile — the defect
// this fixes compiled cleanly and stored nothing.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// True iff an ERROR diagnostic contains every one of `needles`.
bool errorSays(const std::string& program,
               std::initializer_list<const char*> needles) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        bool all = true;
        for (const char* n : needles)
            if (d.message.find(n) == std::string::npos) { all = false; break; }
        if (all) return true;
    }
    return false;
}

// The issue's own class: a getter-only indexer and a getter-only named property
// on one type, so both spellings are measured against the same backing field.
const char* kGetterOnly =
    "module M; "
    "class B "
    "  private _v: int := 7; "
    "  public property [i: int]: int get => _v + i; end; "
    "  public property P: int get => _v; end P; "
    "  public method Dump(): int return _v; end Dump; "
    "end B; ";

const char* kReadWrite =
    "module M; "
    "class RW "
    "  private _v: int := 7; "
    "  public property [i: int]: int "
    "    get => _v + i; "
    "    set => do _v := value; end; "
    "  end; "
    "  public property P: int "
    "    get => _v; "
    "    set => do _v := value; end; "
    "  end P; "
    "  public property A: int; "
    "  public method Dump(): int return _v; end Dump; "
    "end RW; ";

std::string main_(const char* decls, const std::string& body) {
    return std::string(decls) +
           "class C public static method Main(): int " + body +
           " end Main; end C;";
}

int run(const bezel::test::JitResult& r) { return r.ran ? r.exitCode : -999; }

} // namespace

// ===========================================================================
// The two reported rows. Both compiled clean and discarded the write.
// ===========================================================================

// §17.6 / §23.3: `b.P++` on a getter-only NAMED property.
BEZEL_TEST(getter_only_increment_2868,
           named_property_increment_rejected_spec_17_6) {
    BEZEL_EXPECT_TRUE(errorSays(main_(kGetterOnly,
        "  using var b := new B(); b.P++; return b.Dump();"),
        {"increment/decrement", "read-only property `P`", "no setter",
         "17.6", "23.3"}));
}

// §17.6 / §23.3 / §23.4: `b[0]++` on a getter-only INDEXER. The message names
// the construct rather than the symbol, whose name is the synthetic ".this".
BEZEL_TEST(getter_only_increment_2868,
           indexer_increment_rejected_spec_23_4) {
    BEZEL_EXPECT_TRUE(errorSays(main_(kGetterOnly,
        "  using var b := new B(); b[0]++; return b.Dump();"),
        {"increment/decrement through", "read-only indexer", "no setter",
         "17.6", "23.3"}));
}

// §17.6: "The same desugaring applies to `--`" — the decrement spelling of both
// rows reaches the same check.
BEZEL_TEST(getter_only_increment_2868,
           decrement_spelling_rejected_spec_17_6) {
    BEZEL_EXPECT_TRUE(errorSays(main_(kGetterOnly,
        "  using var b := new B(); b.P--; return b.Dump();"),
        {"increment/decrement", "read-only property `P`", "no setter"}));
    BEZEL_EXPECT_TRUE(errorSays(main_(kGetterOnly,
        "  using var b := new B(); b[0]--; return b.Dump();"),
        {"increment/decrement through", "read-only indexer", "no setter"}));
}

// §23.2's expression-bodied form is read-only by shape and reaches the same
// rejection through the `Expression` bodyKind arm rather than the block one.
BEZEL_TEST(getter_only_increment_2868,
           expression_bodied_property_increment_rejected_spec_23_2) {
    BEZEL_EXPECT_TRUE(errorSays(
        "module M; "
        "class E "
        "  private _v: int := 1; "
        "  public property Q: int => _v; "
        "end E; "
        "class C public static method Main(): int "
        "  using var e := new E(); e.Q++; return 0; "
        "end Main; end C;",
        {"increment/decrement", "read-only property `Q`", "no setter"}));
}

// ===========================================================================
// The rest of the shared check, which the increment path also never reached.
// ===========================================================================

// §23.1: a `readonly` auto-property "may be assigned only in a field
// initializer or in a constructor", so `++` outside one is rejected — with
// §23.1's own message, distinct from the no-setter one.
BEZEL_TEST(getter_only_increment_2868,
           readonly_auto_property_increment_outside_ctor_rejected_spec_23_1) {
    BEZEL_EXPECT_TRUE(errorSays(
        "module M; "
        "class R "
        "  public readonly property RA: int; "
        "end R; "
        "class C public static method Main(): int "
        "  using var r := new R(); r.RA++; return 0; "
        "end Main; end C;",
        {"increment/decrement", "readonly property `RA`",
         "field initializer or constructor", "23.1"}));
}

// §13.6: the SETTER's own visibility gates the write. A `private set` property
// incremented from outside the declaring class is rejected — again with its own
// distinct message.
BEZEL_TEST(getter_only_increment_2868,
           private_setter_increment_from_outside_rejected_spec_13_6) {
    BEZEL_EXPECT_TRUE(errorSays(
        "module M; "
        "class S "
        "  public property PS: int private set; "
        "end S; "
        "class C public static method Main(): int "
        "  using var s := new S(); s.PS++; return 0; "
        "end Main; end C;",
        {"increment/decrement", "property `PS`", "not accessible", "13.6"}));
}

// ===========================================================================
// ACCEPT-CONTROLS. A false reject is worse than the bug this fixes, so each of
// these asserts a VALUE.
// ===========================================================================

// A read-write property and indexer must still accept `++` AND `--` and store
// the right number. `b.P++`: get 7, +1, set 8. `b[0]++`: get 8+0, +1, set 9.
// `b[0]--`: get 9, -1, set 8. The auto-property `A` starts at 0 and reaches 1.
BEZEL_TEST(getter_only_increment_2868,
           read_write_property_and_indexer_still_accepted_spec_23_3) {
    BEZEL_EXPECT_EQ(run(runAot(main_(kReadWrite,
        "  using var b := new RW(); "
        "  b.P++;   if b.Dump() <> 8 then return 91; end; "
        "  b[0]++;  if b.Dump() <> 9 then return 92; end; "
        "  b[0]--;  if b.Dump() <> 8 then return 93; end; "
        "  b.P--;   if b.Dump() <> 7 then return 94; end; "
        "  b.A++;   if b.A <> 1 then return 95; end; "
        "  return 0;"))), 0);
}

// A `private set` property incremented from INSIDE its declaring class is legal
// (§13.6) and must still run.
BEZEL_TEST(getter_only_increment_2868,
           private_setter_increment_from_inside_still_accepted_spec_13_6) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class S "
        "  public property PS: int private set; "
        "  public method Bump(): int PS++; return PS; end Bump; "
        "end S; "
        "class C public static method Main(): int "
        "  using var s := new S(); return s.Bump(); "
        "end Main; end C;")), 1);
}

// §23.1's carve-out: a `readonly` auto-property IS assignable in a constructor,
// so `++` there must still be accepted.
BEZEL_TEST(getter_only_increment_2868,
           readonly_auto_property_increment_in_ctor_still_accepted_spec_23_1) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class R "
        "  public readonly property RA: int; "
        "  public constructor() RA := 0; RA++; end constructor; "
        "end R; "
        "class C public static method Main(): int "
        "  using var r := new R(); return r.RA; "
        "end Main; end C;")), 1);
}

// Builtin `Array`/`Dictionary` element increments resolve no user
// PropertySymbol, so they cannot reach the new call at all. Pinned, not argued.
BEZEL_TEST(getter_only_increment_2868,
           builtin_collection_increments_untouched_spec_17_6) {
    BEZEL_EXPECT_EQ(run(runAot(main_(kReadWrite,
        "  using var a := new Array<int>(3, 0); "
        "  a[1] := 5; a[1]++; if a[1] <> 6 then return 91; end; "
        "  using var d := new Dictionary<string, int>(); "
        "  d[\"k\"] := 2; d[\"k\"]++; if d[\"k\"] <> 3 then return 92; end; "
        "  return 0;"))), 0);
}

// ⚠ FLIPPED by fix #3978 (§25.1/§17.6, v0.295). Pre-fix comment (kept for
// history): "§17.6's category (1) ... the statement `x++` is equivalent to
// `x := T.operator++(x)`. The overload must still be selected." §25.1/§17.6
// retire category (1) entirely at v0.295 — `Counter`'s `operator ++`
// declaration is now rejected on its own, and `c++` is independently
// rejected at the use.
BEZEL_TEST(getter_only_increment_2868,
           user_operator_increment_declaration_rejected_spec_25_1_issue_3978) {
    const std::string program =
        "module M; "
        "record Counter(N: int) "
        "  public static method operator ++(c: Counter): Counter "
        "    return new Counter(c.N + 1); "
        "  end; "
        "end Counter; "
        "class C public static method Main(): int "
        "  var c := new Counter(4); c++; return c.N; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(errorSays(program,
        {"operator `++` may not be overloaded", "§25.1"}));
    BEZEL_EXPECT_TRUE(errorSays(program,
        {"`++` is not defined on record type `Counter`", "§17.6"}));
}

// A plain local and a plain writable field are §17.6's other two operand
// categories and resolve no PropertySymbol — untouched.
BEZEL_TEST(getter_only_increment_2868,
           local_and_field_increment_untouched_spec_17_6) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; "
        "class F public _f: int := 0; end F; "
        "class C public static method Main(): int "
        "  using var f := new F(); f._f++; "
        "  var n := 10; n++; "
        "  return n + f._f; "
        "end Main; end C;")), 12);
}

// #2309's positional-record rejection must keep its OWN distinct diagnostic:
// its target resolves to a FieldSymbol, so the shared property check leaves it
// alone and the two messages can never both fire.
BEZEL_TEST(getter_only_increment_2868,
           record_positional_keeps_its_own_diagnostic_spec_9_4) {
    BEZEL_EXPECT_TRUE(errorSays(
        "module M; "
        "record Pt(X: int, Y: int) end Pt; "
        "class C public static method Main(): int "
        "  var p := new Pt(1, 2); p.X++; return 0; "
        "end Main; end C;",
        {"auto-generated property `X`", "positional record", "9.4"}));
    // …and the §23.3 message must NOT also be produced for it.
    BEZEL_EXPECT_FALSE(errorSays(
        "module M; "
        "record Pt(X: int, Y: int) end Pt; "
        "class C public static method Main(): int "
        "  var p := new Pt(1, 2); p.X++; return 0; "
        "end Main; end C;",
        {"no setter"}));
}

// An INTERFACE-typed receiver reaches the property through a bodiless
// PropertyDecl and a builtin-interface indexer resolves a SYNTHESIZED
// PropertySymbol; both must still be accepted and store the right value. This
// is the guard against the shared check over-rejecting the symbol shapes that
// carry no ordinary user `PropertyDecl` body.
BEZEL_TEST(getter_only_increment_2868,
           interface_property_and_builtin_ilist_indexer_accepted_spec_11_2) {
    BEZEL_EXPECT_EQ(run(runAot(
        "module M; import System.Collections.Generic; "
        "interface IHasP property P: int; end IHasP; "
        "class Impl: IHasP "
        "  private _v: int := 4; "
        "  public property P: int "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end P; "
        "end Impl; "
        "class C public static method Main(): int "
        "  using var im := new Impl(); var q: IHasP := im; q.P++; "
        "  if im.P <> 5 then return 91; end; "
        "  using var arr := new Array<int>(3, 0); arr[1] := 5; "
        "  var lst: IList<int> := arr; lst[1]++; "
        "  if arr[1] <> 6 then return 92; end; "
        "  return 0; "
        "end Main; end C;")), 0);
}
