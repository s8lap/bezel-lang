// narrow issue 3440 — `OfType<U>()` performs no runtime type test at all
// (stdlib §5 *Filtering*; spec §8.1 *Definition*, §12 *Inheritance and
// Dispatch*, §12.7 *Type tests*; v0.283).
//
// WHAT SHIPPED BEFORE THIS INTERIM. `LlvmBackend_Calls3.cpp`'s `OfType` arm
// builds a `bz_lazy_where` whose predicate trampoline is a constant
// `ret i32 1`; both sema arms (`ExprTypecheck_Calls.cpp`, the lazy-receiver arm
// and the collection-receiver arm) type the result `IEnumerable<U>` from the
// explicit type argument. So EVERY element was relabelled `U` untested. Two
// source comments each asserted the OTHER side handled it — codegen's "U must
// equal T (statically enforced by sema)" and sema's "when U != T for value
// types, produces an empty enumerator" — and neither did.
//
// Measured on HEAD 1e3861a. Every row COMPILED CLEAN; the "before" column is
// what the program then did at run time.
//
// ⚠ HOW SEVERE A ROW LOOKS DEPENDS ON WHETHER IT *USES* THE RELABELLED ELEMENT,
// and the rows below say which shape was measured. Merely counting a bad
// sequence usually survives; reading a field through the wrong type is what
// faults. The rows that ship in this file mostly count, so most of them did NOT
// crash before the fix — they returned a WRONG COUNT, silently. That is the
// worse direction, not the milder one, and it is why every reject row below
// asserts the REJECTION rather than a pre-fix crash.
//
//   Array<Animal>{Cat}.OfType<Dog>()   count=1, must be 0. Reading the Dog-only
//     (the shape shipped here)         `int` field off the `Cat` is SILENT:
//                                      exit 0, garbage 0. Under ASan the same
//                                      program is a heap-buffer-overflow. #3440
//                                      measured a SIGSEGV for the variant that
//                                      reads a Dog-only `string` field, which
//                                      dereferences the garbage; this file does
//                                      not ship that variant.
//   ... .Where(...).OfType<Dog>()      identical, through the OTHER sema arm
//   Array<int>{1,2,3}.OfType<string>() SIGSEGV even counting — `ToArray()`
//                                      materializes 8-byte pointers out of the
//                                      4-byte int slots (#3440's ASan note)
//   Array<int>{1,2,3}.OfType<double>() count=2, no crash — the int bits read as
//                                      an f64 (5e-324, 1e-323) when used
//   Array<int>.OfType<Object>()        SIGSEGV when used (§8.1: a primitive
//                                      reaches Object by BOXING, not in place)
//   Array<(int)->int>.OfType<Object>() SIGSEGV when used
//   Array<Rec>.OfType<Object>()        count=1 for the counting shape shipped
//                                      here; SIGSEGV for the variant that calls
//                                      `ToString()` on the element
//   Array<Range<int>>.OfType<Object>() SIGSEGV — and this one was ACCEPTED by
//                                      the first landing of this fix; see the
//                                      builtin-class-sym note below
//   Array<Dog?>{null}.OfType<Dog>()    SIGSEGV on the relabelled null
//
// WHAT THIS INTERIM DOES, AND WHAT IT DELIBERATELY DOES NOT.
// It does NOT implement `OfType`: no runtime type test is emitted, and no
// codegen file is touched. It rejects at sema, as NOT YET IMPLEMENTED, the
// pairs for which it can PROVE the identity relabelling wrong, and leaves the
// pairs it can prove right compiling and producing the values they produce
// today. It does not claim to reject *exactly* the wrong pairs: the accepted
// set is an explicit, enumerated list, and the ⚠ note below records that the
// first landing of this fix got that list wrong in one direction.
//
// The accepted set is the pairs whose omitted test could not have failed AND
// which share one runtime representation:
//   * `U` is the sequence's own element type — nothing is relabelled at all;
//   * `U` is a base class or implemented interface of it (spec §12's
//     inheritance/conformance chain), both non-generic — a reference upcast;
//   * `U` is `Object` at a reference-typed element — spec §8.1 *Definition*
//     (v0.283): classes, interfaces and `string` reach `Object` "via their
//     reference-type heap representation", whereas "records, enums, primitives,
//     inline arrays, and the unit type derive via boxing (§8.6) when used in
//     `Object`-typed positions".
// ⚠ `kind == Class` IS NOT A REFERENCE-TYPE TEST, and the first landing of this
// fix shipped that mistake. `SemaContext::builtinClassSym` mints every
// compiler-intrinsic type with `kind = Class`, including ones §8.1 puts on the
// BOXING side, so the third bullet admitted `Range<T>` (a §9.8 body record) and
// `Ptr<T>`. `Array<Range<int>>.OfType<Object>()` was accepted and SIGSEGV'd —
// the accepted set was NOT sound. The codebase already documents this trap and
// carves it out at `LlvmBackend_Setup.cpp` (fix #3127); the predicate now gates
// on `declaration->moduleName.empty()`, the front end's existing builtin-symbol
// discriminator (#2995/#2997, #3022), which admits only user-declared and
// imported classes and interfaces. `reject_range_element_to_object_*` below
// pins it. Recorded rather than quietly repaired, because "one measured
// counterexample in the accepted set" is exactly the thing a reader of this
// file should not have to rediscover.
//
// Beyond that, the accepted set is an UNDER-approximation on purpose: an
// unproven pair draws a "not yet implemented" diagnostic, never a lowering. The
// one measured cost is the GENERIC upcast — `OwningArray<Der<int>>.OfType<Base<int>>()` works today
// (acc=1) and is now refused — and it is refused for a reason, not for tidiness:
// `ExprTC::isSubtype`, the one existing walker of that chain, deliberately
// ignores generic arguments, so the SAME
// judgment that admits `Base<int>` also admits `Base<string>`, and
// `OwningArray<Der<int>>.OfType<Base<string>>()` compiles clean today and
// SIGSEGVs on `b.Tag.Length`. Admitting the sound half needs an argument-
// substitution judgment no existing authority makes; building one here would be
// a fourth spelling of a classification, so the interim declines both. Zero
// occurrences of that shape exist in the repository.
//
// ⚠ THIS REJECTION IS AN INTERPRETATION THE SPEC DOES NOT STATE, recorded per
// CLAUDE.md §4 rather than made silently. `OfType` has NO stated semantics in
// any normative document: `Bezel_Standard_Library.md` carries its bare
// signature under a `// Filtering` group comment plus its name in the
// callback-free-lazy-operator list, and `Bezel_Language_Spec.md` and
// `Bezel_Changelog.md` do not mention it at all (issue 3441). The operator is
// also removal candidate R2 of proposal 0121 (issue 3409, owner ruling
// pending), so this commits to NOTHING about what `OfType` means: it says only
// that this compiler does not implement these cases and will not miscompile
// them. If R2 is accepted the member dies and these rejections with it.
//
// THE TWO ARMS. Two independent sema arms type `OfType` — one for a lazy /
// `Range` / `IGrouping` receiver, one for a concrete collection receiver — and
// #3440 measured the defect reproducing identically through both. A rejection
// wired into only one would read as complete while half the surface still
// miscompiled, so the reject rows below cover both (`..._collection_arm_...`
// and `..._lazy_arm_...` / `..._range_receiver_...`), and so do the controls.
//
// THE ACCEPT-CONTROLS ARE HALF THIS FILE ON PURPOSE, and every one of them
// asserts a VALUE, not "it compiled": a control that only proves the compiler
// did not error cannot tell a preserved lowering from a broken one. They must
// pass on the PRE-fix compiler as well as the post-fix one — that is what makes
// them controls rather than a second copy of the fix.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// The class hierarchy the reject/accept rows share. `Dog` derives from
// `Animal` and implements `IBarker`; `Cat` derives from `Animal` and does not.
// `Widget` and `IUnused` are unrelated to everything.
const char* kTypes = R"BZL(
module M;
import System;
public interface IBarker
  method Bark(): int;
end IBarker;
public interface IUnused
  method Unused(): int;
end IUnused;
public class Animal
  public Tag: int := 1;
  public virtual method Who(): int return 0; end Who;
end Animal;
public class Dog: Animal, IBarker
  public DogOnly: int := 77;
  public override method Who(): int return 1; end Who;
  public method Bark(): int return 7; end Bark;
end Dog;
public class Cat: Animal
  public override method Who(): int return 2; end Who;
end Cat;
public class Widget
  public Label: string := "w";
end Widget;
public record Pt(X: int, Y: int);
public class Box<T>
  public Tag: int := 3;
end Box;
)BZL";

std::string prog(const std::string& mainBody) {
    return std::string(kTypes) +
           "class C\n"
           "  public static method Main(): int\n" +
           mainBody +
           "\n  end Main;\nend C;\n";
}

// True iff SOME error diagnostic is the issue-3440 "not yet implemented" one,
// carrying its operator name, its NOT-IMPLEMENTED framing, its issue pointer
// and its stdlib citation. Asserting the TEXT (not `hasErrors()`) is what
// distinguishes this rule from any other error the program might draw — a
// `hasErrors()` row would pass on an unrelated unknown-member error.
bool rejectedAsNotImplemented(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("`OfType<") == std::string::npos) continue;
        if (d.message.find("is not yet implemented") == std::string::npos)
            continue;
        if (d.message.find("stdlib §5 / issue 3440") == std::string::npos)
            continue;
        if (d.message.find("needs a per-element runtime type test and the "
                           "compiler emits none") == std::string::npos)
            continue;
        return true;
    }
    return false;
}

// The diagnostic must point at the call, never at 0:0.
//
// ⚠ It cannot be checked through `d.location`. On a compile REJECT the AOT
// harness synthesizes exactly ONE diagnostic (`aot_support.hpp`, the
// `CompileOutcome != Ok` arm) whose `location` is a default-constructed
// `SourceLocation` — 0:0 — and whose `message` is the compiler's whole captured
// stderr. So `d.location.line != 0` is VACUOUSLY FALSE for every reject test in
// the suite, and a row asserting it would fail for a reason that has nothing to
// do with the diagnostic under test. The rendered `<file>:<line>:<col>:` prefix
// inside the message is the real location, and it is the one the user sees, so
// that is what this reads: both numbers must be present and non-zero
// immediately before the message text.
bool rejectionIsLocated(const JitResult& r) {
    const std::string tail = ": `OfType<";
    for (const auto& d : r.diagnostics.diagnostics()) {
        const std::string& m = d.message;
        for (std::size_t at = m.find(tail); at != std::string::npos;
             at = m.find(tail, at + 1)) {
            std::size_t p = at;  // index of the ':' that ends "<file>:<line>:<col>"
            auto digitsBack = [&m](std::size_t& q) -> long {
                const std::size_t end = q;
                while (q > 0 && m[q - 1] >= '0' && m[q - 1] <= '9') --q;
                if (q == end) return -1;
                return std::stol(m.substr(q, end - q));
            };
            const long col = digitsBack(p);
            if (col <= 0 || p == 0 || m[p - 1] != ':') continue;
            --p;
            if (digitsBack(p) > 0) return true;
        }
    }
    return false;
}

// The diagnostic names BOTH types it was asked about, so the reader can see
// which pair was refused without re-reading the call.
bool rejectionNames(const JitResult& r, const std::string& u,
                    const std::string& t) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("stdlib §5 / issue 3440") == std::string::npos)
            continue;
        return d.message.find("`OfType<" + u + ">()`") != std::string::npos &&
               d.message.find("sequence of `" + t + "`") != std::string::npos;
    }
    return false;
}

JitResult reject(const std::string& mainBody) { return runAot(prog(mainBody)); }

int run(const std::string& mainBody) {
    auto r = runAot(prog(mainBody));
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ===========================================================================
// REJECT — the unsound cells, COLLECTION-receiver arm
// ===========================================================================

// The canonical downcast, and the reason this is a soundness fix rather than a
// tidiness one: pre-fix this compiled clean, reported one element for an array
// holding zero Dogs, and read `DogOnly` 8 bytes past the end of the `Cat`.
BEZEL_TEST(sema_oftype_3440, reject_downcast_collection_arm_issue_3440) {
    auto r = reject(
        "    using var xs := new OwningArray<Animal>();\n"
        "    xs.Add(new Cat());\n"
        "    var n := 0;\n"
        "    for var d in xs.OfType<Dog>() do n := n + d.DogOnly; end;\n"
        "    return n;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedAsNotImplemented(r));
    BEZEL_EXPECT_TRUE(rejectionIsLocated(r));
    BEZEL_EXPECT_TRUE(rejectionNames(r, "Dog", "Animal"));
}

// A sibling class: `Cat` and `Dog` share a base but neither derives from the
// other, so no element could pass the omitted test.
BEZEL_TEST(sema_oftype_3440, reject_sibling_class_collection_arm_issue_3440) {
    auto r = reject(
        "    using var xs := new OwningArray<Cat>();\n"
        "    xs.Add(new Cat());\n"
        "    using var b := xs.OfType<Dog>().ToArray();\n"
        "    return b.Count;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedAsNotImplemented(r));
}

// An entirely unrelated class. Pre-fix: count=1 and `Label` read off a `Dog`.
BEZEL_TEST(sema_oftype_3440, reject_unrelated_class_issue_3440) {
    auto r = reject(
        "    using var xs := new OwningArray<Dog>();\n"
        "    xs.Add(new Dog());\n"
        "    using var b := xs.OfType<Widget>().ToArray();\n"
        "    return b.Count;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedAsNotImplemented(r));
    BEZEL_EXPECT_TRUE(rejectionNames(r, "Widget", "Dog"));
}

// An interface `Dog` does not implement. The §12 chain walk covers implemented
// interfaces, so this row proves the gate reads that chain rather than only the
// base-class one.
BEZEL_TEST(sema_oftype_3440, reject_unimplemented_interface_issue_3440) {
    auto r = reject(
        "    using var xs := new OwningArray<Dog>();\n"
        "    xs.Add(new Dog());\n"
        "    using var b := xs.OfType<IUnused>().ToArray();\n"
        "    return b.Count;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedAsNotImplemented(r));
}

// Value element, reference `U`: pre-fix this SIGSEGV'd (and, per #3440's ASan
// characterisation, over-read the 4-byte source slot as an 8-byte pointer,
// because the lazy pipeline keeps the SOURCE's element size).
BEZEL_TEST(sema_oftype_3440, reject_int_to_string_issue_3440) {
    auto r = reject(
        "    using var a := new Array<int>();\n"
        "    a.Add(1); a.Add(2); a.Add(3);\n"
        "    using var b := a.OfType<string>().ToArray();\n"
        "    return b.Count;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedAsNotImplemented(r));
    BEZEL_EXPECT_TRUE(rejectionNames(r, "string", "int"));
}

// Value element, value `U` of the same width class: pre-fix this was SILENT —
// no crash, no diagnostic, just the int bits read as an f64 (5e-324, 1e-323).
// Silence is the worst direction, which is why this row exists separately.
BEZEL_TEST(sema_oftype_3440, reject_int_to_double_issue_3440) {
    auto r = reject(
        "    using var a := new Array<int>();\n"
        "    a.Add(1); a.Add(2);\n"
        "    using var b := a.OfType<double>().ToArray();\n"
        "    return b.Count;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedAsNotImplemented(r));
}

// §8.1's boxing clause, the arm the accepted `Object` cases below make it easy
// to get wrong: a PRIMITIVE reaches `Object` by boxing, so relabelling it in
// place is not an upcast. Measured pre-fix: SIGSEGV.
BEZEL_TEST(sema_oftype_3440, reject_primitive_to_object_spec_8_1_issue_3440) {
    auto r = reject(
        "    using var a := new Array<int>();\n"
        "    a.Add(11); a.Add(22);\n"
        "    var n := 0;\n"
        "    for var o in a.OfType<Object>() do n := n + o.ToString().Length; end;\n"
        "    return n;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedAsNotImplemented(r));
    BEZEL_EXPECT_TRUE(rejectionNames(r, "Object", "int"));
}

// The same §8.1 boxing clause at a RECORD element. Measured pre-fix: SIGSEGV.
BEZEL_TEST(sema_oftype_3440, reject_record_to_object_spec_8_1_issue_3440) {
    auto r = reject(
        "    using var a := new Array<Pt>();\n"
        "    a.Add(new Pt(1, 2));\n"
        "    using var b := a.OfType<Object>().ToArray();\n"
        "    return b.Count;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedAsNotImplemented(r));
}

// A NULLABLE source element at a non-nullable `U`. `null` is not an instance of
// `Dog`, so the omitted test would have excluded it; pre-fix the null was
// relabelled and the first member read SIGSEGV'd. Same declaration on both
// sides, so `isSubtype` alone would have called this an upcast — this row pins
// the nullability gate specifically.
BEZEL_TEST(sema_oftype_3440, reject_nullable_source_element_issue_3440) {
    auto r = reject(
        "    using var a := new Array<Dog?>();\n"
        "    a.Add(null);\n"
        "    var n := 0;\n"
        "    for var d in a.OfType<Dog>() do n := n + d.DogOnly; end;\n"
        "    return n;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedAsNotImplemented(r));
}

// `Range<T>` at `U = Object`. THE CELL THE FIRST LANDING OF THIS FIX GOT WRONG:
// it was ACCEPTED and still SIGSEGV'd, because `Range<T>` is a §9.8 body record
// that boxes per §8.6 but whose `TypeSymbol` is a compiler-intrinsic
// `builtinClassSym` carrying `kind == Class` — so the §8.1 arm's
// class-or-interface test called it a reference type. `LlvmBackend_Setup.cpp`
// already documents this exact trap and carves it out (fix #3127); the
// predicate now additionally requires a NON-EMPTY `declaration->moduleName`,
// the front end's existing builtin-symbol discriminator (#2995/#2997, #3022),
// which no intrinsic has. This row is the pin: without it nothing in the suite
// distinguishes a user class from an intrinsic at this arm.
BEZEL_TEST(sema_oftype_3440, reject_range_element_to_object_spec_8_6_issue_3440) {
    auto r = reject(
        "    using var a := new Array<Range<int>>();\n"
        "    a.Add(1..3);\n"
        "    using var b := a.OfType<Object>().ToArray();\n"
        "    return b.Count;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedAsNotImplemented(r));
    BEZEL_EXPECT_TRUE(rejectionIsLocated(r));
}

// The same intrinsic-vs-user distinction from the other side: `Ptr<T>` is also
// pinned to a `builtinClassSym` (`TypeResolution_Names.cpp`) and was also
// accepted by the first landing. It is NOT on `isValueBuiltinByName`'s list,
// which is why that consolidated name predicate was not sufficient here and the
// structural `moduleName` gate was used instead.
BEZEL_TEST(sema_oftype_3440, reject_ptr_element_to_object_issue_3440) {
    auto r = reject(
        "    using var a := new Array<Ptr<int>>();\n"
        "    using var b := a.OfType<Object>().ToArray();\n"
        "    return b.Count;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedAsNotImplemented(r));
}

// ===========================================================================
// REJECT — the unsound cells, LAZY-receiver arm (the SECOND sema arm)
// ===========================================================================

// Identical defect, different arm: a `Where(...)` in front routes the call
// through the lazy-receiver typing arm instead of the collection one. Without
// a gate on THIS arm the fix would be half a fix that reads as a whole one.
BEZEL_TEST(sema_oftype_3440, reject_downcast_lazy_arm_issue_3440) {
    auto r = reject(
        "    using var xs := new OwningArray<Animal>();\n"
        "    xs.Add(new Cat());\n"
        "    var n := 0;\n"
        "    for var d in xs.Where(lambda(x: Animal) => true).OfType<Dog>() do\n"
        "      n := n + d.DogOnly;\n"
        "    end;\n"
        "    return n;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedAsNotImplemented(r));
    BEZEL_EXPECT_TRUE(rejectionIsLocated(r));
    BEZEL_EXPECT_TRUE(rejectionNames(r, "Dog", "Animal"));
}

// The lazy arm at a value element type, reached through `Select`.
BEZEL_TEST(sema_oftype_3440, reject_int_to_string_lazy_arm_issue_3440) {
    auto r = reject(
        "    using var a := new Array<int>();\n"
        "    a.Add(1); a.Add(2);\n"
        "    using var b := a.Select(lambda(x: int) => x + 1)"
        ".OfType<string>().ToArray();\n"
        "    return b.Count;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedAsNotImplemented(r));
}

// A `Range<T>` receiver (§16.3) is routed through the same lazy arm, so it is
// gated by the same predicate.
BEZEL_TEST(sema_oftype_3440, reject_range_receiver_issue_3440) {
    auto r = reject(
        "    var n := 0;\n"
        "    for var s in (1..4).OfType<string>() do n := n + s.Length; end;\n"
        "    return n;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedAsNotImplemented(r));
    BEZEL_EXPECT_TRUE(rejectionNames(r, "string", "int"));
}

// ===========================================================================
// ACCEPT-CONTROLS — the sound cells keep compiling AND keep their values.
// Every row here passes on the PRE-fix compiler too; that is what makes it a
// control. None asserts "it compiled" — each asserts an observable.
// ===========================================================================

// `U` is the sequence's own element type: nothing is relabelled. This is the
// cell the three pre-existing `stdlib_wave2_test.cpp` OfType tests use, pinned
// again here so a future tightening of the gate cannot silently drop it.
BEZEL_TEST(sema_oftype_3440, accept_same_element_type_returns_all) {
    BEZEL_EXPECT_EQ(run("    using var a := new Array<int>();\n"
                        "    a.Add(1); a.Add(2); a.Add(3);\n"
                        "    using var b := a.OfType<int>().ToArray();\n"
                        "    return b.Count;"),
                    3);
}

// A base-class upcast: every element already IS an `Animal`, so admitting the
// whole sequence is the right answer. The oracle is a VALUE — `Tag` read
// through the `Animal` view (5) plus the VIRTUAL `Who()` dispatching to `Dog`'s
// override (1) — so a lowering that returned the right count with the wrong
// objects would still fail.
BEZEL_TEST(sema_oftype_3440, accept_base_class_upcast_spec_12) {
    BEZEL_EXPECT_EQ(run("    using var xs := new OwningArray<Dog>();\n"
                        "    var d := new Dog();\n"
                        "    d.Tag := 5;\n"
                        "    xs.Add(owned d);\n"
                        "    var acc := 0;\n"
                        "    var n := 0;\n"
                        "    for var a in xs.OfType<Animal>() do\n"
                        "      acc := acc + a.Tag + a.Who();\n"
                        "      n := n + 1;\n"
                        "    end;\n"
                        "    return acc * 10 + n;"),
                    61);
}

// An IMPLEMENTED-interface upcast — the other half of the §12 base chain.
// Oracle: the interface method actually dispatches (7), so this is not merely
// "the count survived".
BEZEL_TEST(sema_oftype_3440, accept_implemented_interface_upcast_spec_12) {
    BEZEL_EXPECT_EQ(run("    using var xs := new OwningArray<Dog>();\n"
                        "    xs.Add(new Dog());\n"
                        "    var acc := 0;\n"
                        "    for var b in xs.OfType<IBarker>() do\n"
                        "      acc := acc + b.Bark();\n"
                        "    end;\n"
                        "    return acc;"),
                    7);
}

// `U` = `Object` at a CLASS element — §8.1's reference-type-heap-representation
// clause, the accepted counterpart of the rejected primitive/record rows above.
// Oracle: the virtual `ToString()` reaches `Dog`'s runtime type.
BEZEL_TEST(sema_oftype_3440, accept_class_to_object_spec_8_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    using var xs := new OwningArray<Dog>();\n"
                        "    xs.Add(new Dog());\n"
                        "    for var o in xs.OfType<Object>() do\n"
                        "      Console.WriteLine(o.ToString());\n"
                        "    end;\n"
                        "    return 0;")),
                    "M.Dog");
}

// `U` = `Object` at a `string` element — §8.1 names `string` in the same
// reference-representation group. Oracle: the actual string value survives.
BEZEL_TEST(sema_oftype_3440, accept_string_to_object_spec_8_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    using var a := new Array<string>();\n"
                        "    a.Add(\"ab\");\n"
                        "    for var o in a.OfType<Object>() do\n"
                        "      Console.WriteLine(o.ToString());\n"
                        "    end;\n"
                        "    return 0;")),
                    "ab");
}

// A USER GENERIC class at `U = Object`. The `moduleName` gate added for the
// `Range<T>`/`Ptr<T>` cells above must not catch this: `Box<int>` is generic
// like `Range<int>` and reaches the same arm, but it is user-declared, so §8.1
// puts it on the reference-representation side and it stays accepted.
BEZEL_TEST(sema_oftype_3440, accept_user_generic_class_to_object_spec_8_1) {
    BEZEL_EXPECT_EQ(run("    using var xs := new OwningArray<Box<int>>();\n"
                        "    xs.Add(new Box<int>());\n"
                        "    var acc := 0;\n"
                        "    for var o in xs.OfType<Object>() do\n"
                        "      acc := acc + 1;\n"
                        "    end;\n"
                        "    return acc;"),
                    1);
}

// The LAZY arm's sound cells must survive too — a gate wired into both arms can
// over-reject in either. Same element type, through `Where`.
BEZEL_TEST(sema_oftype_3440, accept_same_element_type_lazy_arm) {
    BEZEL_EXPECT_EQ(run("    using var a := new Array<int>();\n"
                        "    a.Add(1); a.Add(2); a.Add(3); a.Add(4);\n"
                        "    using var b := a.Where(lambda(x: int) => x > 1)"
                        ".OfType<int>().ToArray();\n"
                        "    return b.Count;"),
                    3);
}

// The lazy arm's upcast cell, with the same value oracle as the collection one.
BEZEL_TEST(sema_oftype_3440, accept_base_class_upcast_lazy_arm) {
    BEZEL_EXPECT_EQ(run("    using var xs := new OwningArray<Dog>();\n"
                        "    var d := new Dog();\n"
                        "    d.Tag := 5;\n"
                        "    xs.Add(owned d);\n"
                        "    var acc := 0;\n"
                        "    for var a in xs.Where(lambda(x: Dog) => true)"
                        ".OfType<Animal>() do\n"
                        "      acc := acc + a.Tag + a.Who();\n"
                        "    end;\n"
                        "    return acc;"),
                    6);
}

// A `Range<T>` receiver at its sound cell (§16.3 — the same lazy arm).
BEZEL_TEST(sema_oftype_3440, accept_range_receiver_same_element_type) {
    BEZEL_EXPECT_EQ(run("    var n := 0;\n"
                        "    for var x in (1..4).OfType<int>() do n := n + x; end;\n"
                        "    return n;"),
                    10);
}
