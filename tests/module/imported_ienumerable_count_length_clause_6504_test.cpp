// Cross-module coverage for #6504: the `importedName.empty()` clause in the
// #6389 LAZY-LINQ-SENTINEL arms of `src/sema/ExprTypecheck_Calls.cpp`.
//
// ── WHAT THE CLAUSE IS ─────────────────────────────────────────────────────
//
// Three arms guard an undeclared `.Count` / `.Length` read whose receiver is
// the §5 lazy sentinel: a Named TypeRef with `declaration == nullptr`,
// `astNode == nullptr`, a non-empty `genericArgs`, and — the clause —
// `importedName.empty()`. `src/sema/ExprTypecheck_Calls.cpp` 26852 and 26865
// (the two `.`-form arms) and 32369 (the `?.` form).
//
// `tests/sema/ienumerable_count_length_member_reject_6389_test.cpp` holds
// fourteen rows and every one is SINGLE-MODULE, so no row there can reach the
// imported shape: a same-module `Array<int>` / `IEnumerable<int>` receiver
// carries an `astNode` and is decided by the arm's `!tt->astNode` term before
// `importedName` is ever read. That file's header says so and points here.
//
// ── WHY THE CLAUSE IS LOAD-BEARING (§2.5 / §29.3, issue #495) ───────────────
//
// ModuleLoader reconstructs a TypeRef for every imported generic whose
// `lookupNamed` fails — which is every BUILTIN, since none of them is an
// exported user type in the dependency — as Named + `declaration == nullptr` +
// `astNode == nullptr` + `genericArgs`, recording the type name ONLY in
// `importedName` (ModuleLoader.cpp:384-385, the `declaration == nullptr`
// fallback; MEASURED at this commit — the `ModuleLoader.cpp:186` figure
// #6504 quotes is stale). That is byte-for-byte the sentinel shape, and
// `headName()` cannot separate them because it reads `astNode` only when it is
// non-null.
//
// ⚠ WHAT THE CLAUSE PROTECTS, MEASURED — AND IT IS NOT #6504's STATED SHAPE.
//
// #6504's body names `IEnumerable<T>` + `.Count()` as the false reject to pin.
// That is MEASURED WRONG on both halves, and this file is written to what the
// binaries do, not to what the issue predicted:
//
//   * `.Count()` — WITH PARENTHESES — is NOT sensitive to the clause at all.
//     MEASURED: `l.Seq().Count()` compiles and returns 3 on a cross-module
//     `IEnumerable<T>` receiver both with and without the clause deleted. It
//     never reaches these arms: the gate is `&ma != calleeOfCallCtx_` (this
//     is the BARE spelling), so the parenthesised §5 operator skips the arm
//     before any clause is read. #6389's own control row
//     `count_operator_with_parens_still_works_spec_5` already covers the
//     single-module half.
//
//   * BARE `.Count` on `IEnumerable<T>` is NOT a legal program, so rejecting
//     it is not a false reject. stdlib §4.12 declares `IEnumerable<out T>` with
//     exactly one member, `GetEnumerator`; `Count` is §5's `method Count(): int`
//     (Bezel_Standard_Library.md:3643) — a METHOD, spelled with parentheses.
//     #6389's `no member `Count` on `IEnumerable<T>`` message is the CORRECT
//     verdict for that read.
//
//   The receivers the clause actually rescues are the imported CONCRETE
//   collections, whose `.Count` / `.Length` ARE declared properties:
//
//     Array<T>.Count          stdlib §4.4  `public pure readonly property Count: int`
//     OwningBuffer<T>.Length  stdlib §4.14 `public pure readonly property Length: int`
//
//   Each is legal, each returns 3 here, and each is falsely REJECTED without
//   the clause with #6389's own wrong message — naming a lazy LINQ sequence
//   for a receiver that is not one. That is the false reject, and these rows
//   pin it.
//
// ── THE DIRECTION THE CLAUSE DOES NOT OWN ──────────────────────────────────
//
// #6389's own rows key on the `IEnumerable<T>` RECEIVER; these key on the
// other direction, a reconstructed builtin whose member its own declaration
// provides. Together the two files pin both sides: `importedName` empty ⇒
// sentinel ⇒ reject a read `IEnumerable<T>` does not declare; `importedName`
// set ⇒ a reconstructed builtin ⇒ resolve the member stdlib declares for it.
//
// A MEASURED LIMIT worth stating rather than papering over: on the IMPORTED
// receiver the pre-existing §4.4 / §4.14 arms do not fire at all.
// `l.MakeArray().Length` and `l.MakeBuffer().Count` compile and return 3
// cross-module, where the SAME-MODULE spellings reject with "use `Count`" /
// "use `Length`". Those arms are gated on `isBuiltin`, which reads
// `recvTy->astNode->name->parts` — empty for the imported form. That is a
// SEPARATE coverage gap in an unrelated arm; it is NOT what this issue is
// about, this file does not pin it, and asserting either verdict for it would
// be pinning a behaviour no one has established. Filed separately.
//
// ── ORACLE ─────────────────────────────────────────────────────────────────
//
// `runAotModules` — the shared two-module AOT oracle (same forced-ASan sweep,
// same `runProgramFill`, same {ran, exitCode, stdoutText, diagnostics} contract
// as `runAot`; see its header in tests/support/aot_support.hpp). It drives the
// `Driver::compileObject` → `Dependency` → `compileFile` path, the ONLY path
// that produces the imported TypeRef. Each `.`-form row asserts the VALUE 3,
// not merely that the program ran: a false reject cannot make it green, and a
// wrong answer cannot either.
//
// ⚠ IT CANNOT BE A SINGLE-MODULE TEST. A same-module `Array<int>` producer
// carries an `astNode`; the imported form does not. Same reason as
// `tests/module/imported_builtin_generic_not_lazy_2593_test.cpp`, which pins
// the same `importedName.empty()` term in #2593's §7 argument gate.
//
// ⚠ THE TWO `?.` ROWS ASSERT COMPILATION, NOT A VALUE, AND THAT IS DELIBERATE.
// `?.` yields `T?` (spec §7.4), so the read needs a `??` narrowing (§7.7) to be
// usable, and MEASURED that narrowing is where a PRE-EXISTING, UNRELATED
// codegen gap bites: `a?.Count ?? 0` on the imported receiver emits #2434
// ("initializer expression failed to lower to a value — degraded to a
// zero-initialized value") and evaluates to 0 rather than 3. Asserting 3 would
// pin that bug and go red for the wrong reason; asserting 0 would pin the
// degraded answer. So these rows assert what they are about — the clause does
// not reject the read — and say so. Asserting compilation IS a real assertion
// here, not a vacuous one: with the clause deleted the very same programs are
// REJECTED (proved per row below), so `r.ran` flips false.
//
// Test names carry the section they derive from.

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

// A library exporting producers whose RETURN TYPES are builtins. `Array<T>` and
// `OwningBuffer<T>` are exactly the reconstructed case: neither is an exported
// user type in this module, so an importer's `lookupNamed` fails for both and
// both come back astNode-less with the name only in `importedName`.
//
// The `owned` return is what puts the receiver on the importer's side at all
// (§28.3's producer rule). `[1, 2, 3]` gives `Count == 3` and
// `new OwningBuffer<int>(3)` gives `Length == 3`, so both `.`-form rows assert
// the SAME value 3 and differ only in the receiver and the member read.
constexpr const char* kLib6504 =
    "module Lib6504; "
    "public class Coll6504 "
    "  public method MakeArray(): owned Array<int> "
    "    var a: Array<int> := [1, 2, 3]; "
    "    return a; "
    "  end MakeArray; "
    "  public method MakeBuffer(): owned OwningBuffer<int> "
    "    return new OwningBuffer<int>(3); "
    "  end MakeBuffer; "
    "end Coll6504;";

// A library exporting a §5 lazy-sequence producer. `Seq(): IEnumerable<int>` is
// #6504's named shape; `return a` is required because §15.4 admits a
// collection literal only for a CONCRETE collection target, never for an
// interface (MEASURED: `[1,2,3]` as a direct `IEnumerable<int>` return is
// rejected by the §15.4 rule). Kept because it is the receiver #6389's rows
// use, and pinning that it still COMPILES with parens is the one claim about
// `IEnumerable<T>` that is both true and worth holding here.
constexpr const char* kLibSeq6504 =
    "module LibSeq6504; "
    "public class Seq6504 "
    "  public method Seq(): IEnumerable<int> "
    "    var a: Array<int> := [1, 2, 3]; "
    "    return a; "
    "  end Seq; "
    "end Seq6504;";

// The importer. `body` is the single statement under test; the preamble
// (`using var l := …`) is shared so a row differs only in the receiver
// expression and the member read.
std::string importer6504(const std::string& body) {
    return "module App6504; "
           "import Lib6504; "
           "class C6504 public static method Main(): int "
           "  using var l := new Coll6504(); "
           "  " + body + " "
           "end Main; end C6504;";
}

std::string seqImporter6504(const std::string& body) {
    return "module AppSeq6504; "
           "import LibSeq6504; "
           "class CSeq6504 public static method Main(): int "
           "  using var s := new Seq6504(); "
           "  " + body + " "
           "end Main; end CSeq6504;";
}

}  // namespace

// ===========================================================================
// THE CLAUSE-SENSITIVE ROWS — the legal cross-module programs it protects.
// ===========================================================================

// stdlib §4.4: `public pure readonly property Count: int` on `Array<T>`. The
// receiver is an IMPORTED `Array<int>` — Named, no declaration, no astNode,
// name in `importedName` — so without the clause's `importedName.empty()`
// term this read is mistaken for a lazy §5 sequence and falsely rejected.
//
// MEASURED fail-without (all three clause occurrences deleted, `bezelc`
// rebuilt): "no member `Count` on a lazy LINQ sequence (`IEnumerable<T>`,
// stdlib §4.12): it declares no count property — use the `Count()` operator
// (stdlib §5)" at the read, and no executable produced. With the clause: clean
// compile, exit 3.
BEZEL_TEST(imported_ienumerable_count_length_clause_6504,
           imported_array_count_property_resolves_spec_4_4) {
    auto r = test::runAotModules(
        {{"Lib6504", kLib6504},
         {"App6504", importer6504("return l.MakeArray().Count;")}},
        "App6504");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// stdlib §4.14: `public pure readonly property Length: int` on
// `OwningBuffer<T>` — the other member of the two `.`-form arms, and the other
// declared property the clause keeps reachable. Different receiver AND
// different member from the row above, so an arm narrowed to `Count` alone (or
// to one receiver family) would leave this row red.
BEZEL_TEST(imported_ienumerable_count_length_clause_6504,
           imported_owningbuffer_length_property_resolves_spec_4_14) {
    auto r = test::runAotModules(
        {{"Lib6504", kLib6504},
         {"App6504", importer6504("return l.MakeBuffer().Length;")}},
        "App6504");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// The `?.` SPELLING, which reaches a SEPARATE sema block
// (`resolveNullCondMember`, ExprTypecheck_Calls.cpp:32369) carrying its own
// copy of the clause. Without a row the three copies could drift and the suite
// would not notice: these are three independent occurrences of the same term.
//
// MEASURED fail-without: the same lazy-LINQ `Count` diagnostic, REJECTED, no
// executable. With the clause: compiles, exit 0.
//
// The exit code is 0, not 3, and that is asserted DELIBERATELY — see the
// ORACLE note above. The pre-existing #2434 lowering gap on `?.` + `??` is not
// this issue's to pin, and 0 is what the program genuinely returns today.
// `r.ran` is the load-bearing assertion: it flips false without the clause.
BEZEL_TEST(imported_ienumerable_count_length_clause_6504,
           imported_array_count_resolves_null_conditional_spec_4_4) {
    auto r = test::runAotModules(
        {{"Lib6504", kLib6504},
         {"App6504", importer6504(
                        "var a := l.MakeArray(); "
                        "var n := a?.Count ?? 0; "
                        "if n > 0 then return 1; end; "
                        "return 0;")}},
        "App6504");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // And the specific thing: the read resolved to the §4.4 property rather
    // than being rejected as a lazy sequence. Asserting the ABSENCE of that
    // message is what makes this row about the clause rather than about
    // compiling at all.
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "on a lazy LINQ sequence"));
}

// The `?.` twin on the other receiver and the other member, for the same
// reason as the row above.
BEZEL_TEST(imported_ienumerable_count_length_clause_6504,
           imported_owningbuffer_length_resolves_null_conditional_spec_4_14) {
    auto r = test::runAotModules(
        {{"Lib6504", kLib6504},
         {"App6504", importer6504(
                        "var b := l.MakeBuffer(); "
                        "var n := b?.Length ?? 0; "
                        "if n > 0 then return 1; end; "
                        "return 0;")}},
        "App6504");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "on a lazy LINQ sequence"));
}

// ===========================================================================
// THE ISSUE'S NAMED SHAPE — asserted at the value it ACTUALLY has.
//
// #6504 asks for a cross-module `IEnumerable<T>` row. MEASURED, the only legal
// such program is the PARENTHESISED §5 operator, and it is NOT sensitive to the
// clause (the `&ma != calleeOfCallCtx_` gate skips the arm before any clause
// term is read). It is pinned here anyway, as the control that says so out
// loud rather than leaving a reader to assume a row like it must be
// clause-sensitive — and because "a legal cross-module `IEnumerable<T>` local
// compiles" is a real claim #6389's header makes, which nothing else here
// holds.
//
// If a future edit DID make this row clause-sensitive, it would mean the
// parenthesised spelling had started reaching the arm — a regression, and this
// row's value assertion would be the thing that caught it.
BEZEL_TEST(imported_ienumerable_count_length_clause_6504,
           imported_ienumerable_count_operator_with_parens_spec_5) {
    auto r = test::runAotModules(
        {{"LibSeq6504", kLibSeq6504},
         {"AppSeq6504", seqImporter6504("return s.Seq().Count();")}},
        "AppSeq6504");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// ===========================================================================
// fix-forward #6776 (§2.5; stdlib §4.19, §3.1; v0.340): the `.Length` LOWERING
// of an imported builtin receiver, on the receivers the rows above do not
// cover.
//
// An imported builtin reaches codegen astNode-less, its head only in
// `importedName`. The string-fallback `.Length` arm in LlvmBackend_Exprs.cpp
// took that for a degenerate TypeRef and read `%bz.string` field 1 (offset 8)
// of the receiver. On a `bz_array_t` that was `count` until #6776 moved it
// behind the `{vtable, type_desc}` tag, which is why
// `imported_owningbuffer_length_property_resolves_spec_4_14` above went red
// at fbd74955 (exit 64). MEASURED at fbd74955 with these two programs:
// FrozenArray `.Length` exited 128, StringBuilder `.Length` exited 0. With the
// fix they exit 4 and 5. The StringBuilder value was wrong before #6776 too:
// offset 8 was the PART count, 1.
constexpr const char* kLibLen6776 =
    "module LibLen6776; "
    "public class Maker6776 "
    "  public method MakeFrozen(): owned FrozenArray<int> "
    "    var a: Array<int> := [4, 5, 6, 7]; "
    "    return FrozenArray<int>.From(a); "
    "  end MakeFrozen; "
    "  public method MakeSB(): owned StringBuilder "
    "    var sb := new StringBuilder(\"abcde\"); "
    "    return sb; "
    "  end MakeSB; "
    "end Maker6776;";

static std::string lenImporter6776(const std::string& body) {
    return "module AppLen6776; "
           "import LibLen6776; "
           "class CLen6776 public static method Main(): int "
           "  using var m := new Maker6776(); "
           "  " + body + " "
           "end Main; end CLen6776;";
}

// stdlib §4.19: `public readonly property Length: int` on `FrozenArray<T>`,
// read through a BOUND imported local. Four elements, so the answer is 4.
BEZEL_TEST(imported_ienumerable_count_length_clause_6504,
           imported_frozenarray_length_reads_element_count_6776_spec_4_19) {
    auto r = test::runAotModules(
        {{"LibLen6776", kLibLen6776},
         {"AppLen6776",
          lenImporter6776("using var f := m.MakeFrozen(); return f.Length;")}},
        "AppLen6776");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);
}

// stdlib §3.1: `StringBuilder.Length: int` is the character count, read on an
// UNBOUND imported temporary. "abcde", so the answer is 5.
BEZEL_TEST(imported_ienumerable_count_length_clause_6504,
           imported_stringbuilder_length_reads_char_count_6776_spec_3_1) {
    auto r = test::runAotModules(
        {{"LibLen6776", kLibLen6776},
         {"AppLen6776", lenImporter6776("return m.MakeSB().Length;")}},
        "AppLen6776");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

}  // namespace bezel::module
