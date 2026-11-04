// Spec-derived regression test for issue #8136 — `new DictionaryEntry<K, V>(k, v)`
// IS REFUSED although the standard library declares a positional record
// (stdlib §4.8; language spec §9.1 / §17.7, v0.355).
//
// THE DECLARATION (stdlib §4.8 `Dictionary<K, V>`, the §4.8 code block):
//
//     public record DictionaryEntry<K, V>(Key: K, Value: V);
//
// Language spec §9.1 ("Positional records"), verbatim:
//
//     "For each positional record, the compiler generates:
//      - A primary constructor whose parameters are the positional fields, in
//        declaration order.
//      - Public read-only properties, one per positional field, with the same
//        names — auto-properties in §23.1's sense, assignable in a constructor
//        of the declaring record under that section's context.
//      …"
//
// ⇒ The declared constructor surface of `DictionaryEntry<K, V>` is exactly
// `new DictionaryEntry<K, V>(Key, Value)`, and the values it produces are the
// record's own two fields.
//
// THE DEFECT (issue #8136, reproduced on staging r30-c1):
//
//     module Conf;
//     public class Program
//       public static method Main()
//         var e := new DictionaryEntry<string, int>("ab", 5);
//         Console.WriteLine(e.Key);
//         Console.WriteLine(e.Value);
//       end Main;
//     end Program;
//
// was REFUSED with
//
//     `new DictionaryEntry(…)` produced no value, so it cannot initialize this
//     local. `DictionaryEntry` has no user declaration, so its CONSTRUCTOR
//     surface is exactly what the standard library documents for it: … (#2974)
//
// — an OVER-REJECTION: the surface the standard library documents IS the
// `new DictionaryEntry<K, V>(Key, Value)` call the program writes. The
// diagnostic is the #2974 gate (`Emitter::unconstructedBuiltinNewName`,
// src/codegen/LlvmBackend_Setup.cpp), which fires because `lowerNew` had NO
// arm for the type and the construction therefore produced no value.
//
// THE FIX, IN FOUR HALVES.
//
//  1. THE SURFACE. `DictionaryEntry<K, V>` joins `ExceptionInfo` (§31.4) and
//     `ReadOnlyStringSpan` (stdlib §3.3) in the registry's arm-2 set — the
//     builtin RECORDS whose only declared constructor is the positional primary
//     one — with its DECLARED ARITY (2). §17.7 then rejects every argument
//     list that is not the declared one: the parameterless form and any
//     wrong-arity form are compile errors, exactly as for an identically-shaped
//     USER record. Those rows are pinned below.
//
//  2. THE ARGUMENTS. The positional primary constructor's parameters are the
//     record's own TYPE PARAMETERS (`Key: K`, `Value: V`), which the
//     parameter table's fixed `kinds` vocabulary cannot express. The
//     argument types are therefore checked against the CONCRETE K and V of
//     the construction's own target type, by `checkAssignable` (§5.1's
//     arithmetic). Without that, `new DictionaryEntry<string, int>(5, "x")`
//     would reach codegen and store an `i32` into the Key's `ptr` slot — an
//     LLVM IR-verifier abort, and (had the widths matched) a SILENT field
//     permutation.
//
//  3. THE CONSTRUCTION. `lowerNew` grows the arm that builds the record's own
//     `{K, V}` layout — the two-field struct `lowerTypeRef` gives the type and
//     the field-read path (`e.Key` / `e.Value`) GEPs into. The arguments
//     resolve BY NAME (§22.5: "A record's positional constructor takes its
//     field names as parameter names", §9.1), so
//     `new DictionaryEntry<string, int>(Value := 5, Key := "ab")` is the same
//     value as the positional spelling.
//
//  4. THE OWNERSHIP. A `string` component's reference count has to balance —
//     see the ownership section below for the model, the two halves that have
//     to land together (the construction's retain, the record's scope-end
//     release), and the escape walk's agreement with them.
//
// THE OWNERSHIP OF A `string` COMPONENT. §9.7: "Reference-typed fields in
// records are always borrows; the record does not own what they point to", and
// §28.9: a `string`'s lifetime "is managed by the runtime without user
// involvement" — "how the runtime manages their lifecycle (reference counting,
// region allocation, or other) is not part of this specification". THIS runtime
// chose reference counting, and reference counting has to be SYMMETRIC: the
// construction takes a reference of its OWN on a `string` component (retaining
// it, or taking the +1 a +1-producer hands over — the same rule
// `lowerStringFieldStore` applies to a USER record's field), and the record's
// scope end gives it back (the `dictEntryKeyRelease` / `dictEntryValueRelease`
// disposals, registered for a provably non-escaping local).
//
// TWO HALVES HAVE TO LAND TOGETHER, or the reference count is off by one:
//
//   * THE MINT. The construction retains a BORROW argument (a local, a
//     parameter, a field read) and TRANSFERS a +1 PRODUCER's single reference
//     into the record — §28.9's owner model, "every owner holds its own
//     reference". The source of a borrow argument therefore KEEPS its own
//     reference and its own scope-end release.
//
//   * THE DISCHARGE. The record's scope end releases the record's own
//     reference.
//
// The escape walk has to agree with the mint: a `new DictionaryEntry(…)`
// argument position is a CALL-BOUNDED BORROW of the source (the §31.4
// built-in-exception constructor's shape, which the walker already admits),
// not an escape — so the source's own scope-end release stays armed. Without
// that half the source's +1 is orphaned (a leak), and with a mint but no
// discharge the record's own +1 leaks.
//
// The ownership rows below therefore use a RUNTIME-PRODUCED string (a
// concatenation), not a literal: a literal is an immortal sentinel
// (`BZ_STRING_RC_SENTINEL` — "static/literal — never freed"), so a literal
// would make the row VACUOUS (it would pass under every ownership model,
// including a broken one). The runtime-produced string is what makes the row
// able to see a missing retain, a missing discharge, or a double free.
//
// ⚠ THE ORACLE TRAP. A use-after-free ABORTS the process before
// LeakSanitizer prints its summary, so a leak assertion alone also reads as
// "clean" for a crash. Every leak row is therefore paired with a crash-oracle
// row that asserts the program RAN TO COMPLETION (`r.ran`) and printed the
// expected bytes. Every leak row goes through `runAotChecked` (which hardcodes
// `detect_leaks = true`); `runAotCheckedLenient` passes `detect_leaks = 0`, so a
// leak assertion written against it would be VACUOUS.
//
// ⚠ THE OTHER ORACLE TRAP. `runAot` fills `diagnostics` only on a REJECT, so
// "this program draws no diagnostic X" is VACUOUS on a program that compiles
// (scripts/check_vacuous_tests.py). The §28.3 auto-drop note is a WARNING on
// a COMPILING program, so it is observed through the OWNERSHIP pass
// (`parseAndCheckOwnership`), which reports warnings — not through `runAot`.

#include <initializer_list>
#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"  // parseAndCheckOwnership, hasAutoDropWarning
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::JitResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The issue's repro, VERBATIM.
std::string reproProgram() {
    return
        "module Conf;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var e := new DictionaryEntry<string, int>(\"ab\", 5);\n"
        "    Console.WriteLine(e.Key);\n"
        "    Console.WriteLine(e.Value);\n"
        "  end Main;\n"
        "end Program;\n";
}

// A program whose local is bound to the given construction.
std::string ctorProgram(const std::string& ctor) {
    return
        "module M;\n"
        "public class C\n"
        "  public static method Main()\n"
        "    var e := " + ctor + ";\n"
        "    Console.WriteLine(e.Key);\n"
        "    Console.WriteLine(e.Value);\n"
        "  end Main;\n"
        "end C;\n";
}

// An ERROR-severity diagnostic whose message carries `needle`.
bool hasError(const std::string& program, const char* needle) {
    JitResult r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The §17.7 diagnostic shape #2994 grew and #3136 generalised: a built-in
// record whose only declared constructor is its positional primary one
// refuses an argument list that does not match the declared arity. NOT
// `hasErrors()`: `runAot` reports a runtime SIGSEGV as an Error too, and the
// rule's own text is what distinguishes the rejection from a crash.
bool rejectsWrongArity(const std::string& program) {
    return hasError(program, "is not a declared constructor of") &&
           hasError(program, "\xC2\xA7""17.7");
}

// A LEAK-oracle row: strict LSan armed, and the program must run clean.
// `expected` lists the substrings the program's output must carry — one per
// line the program prints (`Console.WriteLine` terminates each with a newline,
// so the two outputs are NOT one contiguous substring).
void expectNoLeak(const std::string& program,
                  std::initializer_list<const char*> expected) {
    JitResult r = runAotChecked(program);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    for (const char* e : expected)
        BEZEL_EXPECT_TRUE(r.stdoutText.find(e) != std::string::npos);
    for (const auto& d : r.diagnostics.diagnostics())
        BEZEL_EXPECT_TRUE(d.message.find("byte(s) leaked") == std::string::npos);
}

// A CRASH-oracle row: the program must RUN TO COMPLETION, print `expected`,
// and exit 0. A leak row alone cannot prove the absence of a use-after-free
// (a UAF aborts before LSan prints its summary, so it reads as 0 leaked).
void expectRunsClean(const std::string& program,
                     std::initializer_list<const char*> expected) {
    JitResult r = runAotCheckedLenient(program);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    for (const char* e : expected)
        BEZEL_EXPECT_TRUE(r.stdoutText.find(e) != std::string::npos);
}

// A runtime-produced string, NOT a literal: a literal is an immortal sentinel
// (`BZ_STRING_RC_SENTINEL`), so a literal could not tell a balanced reference
// count from an unbalanced one.
std::string producedKey() { return "\"ab\" + \"c\""; }
std::string producedValue() { return "\"de\" + \"f\""; }

// The §28.3 auto-drop note's POSITIVE CONTROL: the same local NAME (`e`),
// bound to an OWNED class value, DOES draw the note. Without this row the
// "no note" assertion below could pass because the ownership pass stopped
// reporting the note at all.
std::string ownedControlProgram() {
    return
        "module M;\n"
        "public class Res : IDisposable\n"
        "  public Id: int;\n"
        "  public constructor(id: int) Id := id; end constructor;\n"
        "  destructor Console.WriteLine(\"D\"); end destructor;\n"
        "end Res;\n"
        "public class C\n"
        "  public static method Main()\n"
        "    var e := new Res(1);\n"
        "    Console.WriteLine(e.Id);\n"
        "  end Main;\n"
        "end C;\n";
}

}  // namespace

// ── THE REPRO ───────────────────────────────────────────────────────────────

BEZEL_TEST(dictionary_entry_ctor_8136,
           the_repro_constructs_and_prints_the_key_and_the_value_spec_9_1) {
    JitResult r = runAot(reproProgram());
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // The over-rejection is GONE: on the pre-fix build the #2974 gate
    // REJECTED this program ("`new DictionaryEntry(…)` produced no value, so
    // it cannot initialize this local"), so `hasErrors()` is the assertion
    // that is RED there and GREEN here.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("ab") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("5") != std::string::npos);
}

// ── FIELD READS ─────────────────────────────────────────────────────────────

BEZEL_TEST(dictionary_entry_ctor_8136,
           the_two_declared_fields_are_readable_spec_9_1) {
    // §9.1: "Public read-only properties, one per positional field, with the
    // same names". The Key is field 0 and the Value is field 1 of the
    // record's `{K, V}` layout — the layout `lowerTypeRef` gives the type and
    // the field-read path GEPs into.
    JitResult r = runAot(ctorProgram(
        "new DictionaryEntry<string, int>(\"ab\", 5)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("ab") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("5") != std::string::npos);
}

BEZEL_TEST(dictionary_entry_ctor_8136,
           the_named_argument_spelling_is_the_same_value_spec_22_5) {
    // §22.5 + §9.1: a record's positional constructor takes its FIELD names as
    // parameter names, so `Value := 5, Key := "ab"` is the same value as the
    // positional spelling — and NOT a silent field permutation.
    JitResult r = runAot(ctorProgram(
        "new DictionaryEntry<string, int>(Value := 5, Key := \"ab\")"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("ab") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("5") != std::string::npos);
}

// ── THE CONSTRUCTION IS A VALUE (the §28.3 auto-drop note) ──────────────────

BEZEL_TEST(dictionary_entry_ctor_8136,
           the_construction_carries_no_disposal_obligation_spec_28_3_spec_9) {
    // §9: "A record is a value type"; §28.3: a value with nothing to dispose
    // carries no auto-drop obligation. Before the fix the ownership pass
    // classified `new DictionaryEntry(…)` as OWNED (the construction is the
    // canonical producer of an owned value — but only for a CLASS), so it
    // drew the §28.3 must-use / auto-drop note on a value with nothing to
    // dispose. The note is a WARNING on a COMPILING program, so it is read
    // through the OWNERSHIP pass — `runAot` fills its diagnostics only on a
    // reject, which would make the assertion vacuous.
    auto r = parseAndCheckOwnership(ctorProgram(
        "new DictionaryEntry<string, int>(\"ab\", 5)"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "e"));
    // The POSITIVE CONTROL: the same local NAME, bound to an OWNED class
    // value, DOES draw the note — so the assertion above is not vacuous.
    auto ctl = parseAndCheckOwnership(ownedControlProgram());
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(ctl, "e"));
}

// ── OWNERSHIP: a string key and a string value ─────────────────────────────

BEZEL_TEST(dictionary_entry_ctor_8136,
           a_string_key_and_value_leak_nothing_spec_28_9_spec_9_7) {
    // 100 iterations of the construction, each with a RUNTIME-PRODUCED string
    // key and a RUNTIME-PRODUCED string value: the reference counts have to
    // balance exactly (the construction's retain, the producer's discharge,
    // and the record's scope-end release), or this leaks.
    std::string program =
        "module M;\n"
        "public class C\n"
        "  public static method Main()\n"
        "    var i := 0;\n"
        "    while i < 100 do\n"
        "      var e := new DictionaryEntry<string, string>(" +
        producedKey() + ", " + producedValue() + ");\n"
        "      Console.WriteLine(e.Key);\n"
        "      Console.WriteLine(e.Value);\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "  end Main;\n"
        "end C;\n";
    expectNoLeak(program, {"abc", "def"});
    // The CRASH oracle on the same program — a leak row alone cannot prove
    // the absence of a use-after-free or a double free.
    expectRunsClean(program, {"abc", "def"});
}

BEZEL_TEST(dictionary_entry_ctor_8136,
           a_string_key_and_value_from_locals_leak_nothing_spec_28_9) {
    // The same construction, but the components come from NAMED locals that
    // own their own reference. The construction must take a reference of its
    // own (a retain) rather than consuming the local's, and the local must
    // keep its own scope-end release — the two halves of §28.9's owner model.
    // If the construction consumed the local's reference instead, the local's
    // own release would drive the count negative; if the escape walk treated
    // the argument position as an escape, the local's release would be dropped
    // and its reference orphaned (a leak).
    std::string program =
        "module M;\n"
        "public class C\n"
        "  public static method Main()\n"
        "    var i := 0;\n"
        "    while i < 100 do\n"
        "      var k := " + producedKey() + ";\n"
        "      var v := " + producedValue() + ";\n"
        "      var e := new DictionaryEntry<string, string>(k, v);\n"
        "      Console.WriteLine(e.Key);\n"
        "      Console.WriteLine(e.Value);\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "  end Main;\n"
        "end C;\n";
    expectNoLeak(program, {"abc", "def"});
    expectRunsClean(program, {"abc", "def"});
}

// ── THE WRONG ARITY IS STILL REJECTED ───────────────────────────────────────

BEZEL_TEST(dictionary_entry_ctor_8136,
           the_wrong_arity_is_rejected_spec_17_7_stdlib_4_8) {
    // §17.7: "the constructor surface is exactly what that document declares
    // — `new T(...)` resolves only against a declared constructor, never
    // against a compiler-supplied fallback." The stdlib declares exactly TWO
    // positional fields, so the parameterless form and every other arity are
    // compile errors — exactly as for an identically-shaped user record.
    BEZEL_EXPECT_TRUE(rejectsWrongArity(
        ctorProgram("new DictionaryEntry<string, int>()")));
    BEZEL_EXPECT_TRUE(rejectsWrongArity(
        ctorProgram("new DictionaryEntry<string, int>(\"ab\")")));
    BEZEL_EXPECT_TRUE(rejectsWrongArity(
        ctorProgram("new DictionaryEntry<string, int>(\"ab\", 5, 6)")));
}

BEZEL_TEST(dictionary_entry_ctor_8136,
           the_declared_arity_is_admitted_spec_17_7_stdlib_4_8) {
    // The positive control for the row above: the DECLARED arity is admitted,
    // so the rejection above is the arity rule and not a blanket refusal.
    JitResult r = runAot(
        ctorProgram("new DictionaryEntry<string, int>(\"ab\", 5)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectsWrongArity(
        ctorProgram("new DictionaryEntry<string, int>(\"ab\", 5)")));
}

// ── THE ARGUMENT TYPES ──────────────────────────────────────────────────────

BEZEL_TEST(dictionary_entry_ctor_8136,
           an_argument_of_the_wrong_type_is_rejected_spec_5_1) {
    // The parameter types are the record's own TYPE PARAMETERS (`Key: K`,
    // `Value: V`), so the argument types are checked against the CONCRETE K
    // and V of this construction's target type. Without that check the `5`
    // would be stored into the Key's `ptr` slot — an LLVM IR-verifier abort.
    // The needle is the check's own `what` — "argument to `DictionaryEntry`
    // constructor" — so the row cannot pass on some other rule's diagnostic.
    BEZEL_EXPECT_TRUE(hasError(
        ctorProgram("new DictionaryEntry<string, int>(5, \"ab\")"),
        "DictionaryEntry` constructor"));
    BEZEL_EXPECT_TRUE(hasError(
        ctorProgram("new DictionaryEntry<string, int>(5, \"ab\")"),
        "\xC2\xA7""5.1"));
}
