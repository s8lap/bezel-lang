// fix #2958 — extend the §15.4 collection-literal target gate to the
// FIELD-INITIALIZER and AUTO-PROPERTY-INITIALIZER positions (spec §15.4
// *Collection literals*, §22.8 *Field initializers*, §23.1 *Properties* —
// v0.240).
//
// WHAT THE SPEC REQUIRES.  §15.4 states the rule positively and closes it:
//
//   "A collection literal has no standalone type. It is well-typed only in a
//    context that provides the target type, either via type annotation or via
//    the parameter type of a method call."
//
//   "The target types that admit collection literals are: Inline arrays `[N]T`,
//    …  `Array<T>` …  `OwningArray<T>` … `Set<T>` … `LinkedList<T>` …
//    `Stack<T>` … `Queue<T>` … `Dictionary<K, V>` … Any user-defined type
//    declaring the `FromCollectionLiteral` static factory".
//
// `Object`, `int`, `string`, a class with no `FromCollectionLiteral`, or a
// bare type parameter are not on that list. §22.8 governs field initializers
// ("at construction time") and §23.1 makes an auto-property initializer
// initialize the same backing field — so the same §15.4 closed-list rule that
// `var o: Object := [1, 2, 3]` (rejected by #2914) and `Take([1, 2, 3])` /
// `return [1, 2, 3]` (rejected by #3031's argument / return extension) already
// enforce has to apply at `public Slot: Object := [1, 2, 3]` too.
//
// WHY #2914 AND #3031 DID NOT COVER THIS.  The local-decl arm gained the
// positive `isCollectionLiteralAdmissibleTarget` gate in #2914 and #3031
// extended that gate to the call-arg / return / lambda-body positions.  The
// FIELD-INITIALIZER arm (visitMember's FieldDecl arm at ExprTypecheck.cpp
// ~8919) only ran `checkAssignable`, which no-ops on a null `from` (an
// untyped collection literal — the §15.4 carve-out) and therefore silently
// let every non-admissible target through; codegen's `materializeHeap*` path
// then returned null on the bad target and the slot was filled with a zero /
// null value (the WORST of the three #2958 siblings — zero diagnostics,
// silently wrong value).  The auto-property arm mirrored the field arm and so
// inherited the same hole.
//
// MEASURED on 671726f (the compiler this fix was written against), all with
// NO user-facing diagnostic and an exit code of `h.Slot is Object ? 7 : 3`
// returning 3 — i.e. the slot holds the null reference:
//
//   class H { public Slot: Object := [1, 2, 3]; public constructor() end; end;
//   using var h: H := new H();  if h.Slot is Object then return 7; end;
//   return 3;                                                       // exits 3
//
//   class H { public Slot: int := [1, 2, 3]; … }                    // exits 0
//   class H { public Slot: string := [1, 2, 3]; … }                  // exits 1
//   class H { public property P: Object := [1, 2, 3]; … }           // exits 3
//
// THE FIX mirrors the local-decl / return / call-arg gate at the
// FieldDecl and PropertyDecl arms — the SAME `isCollectionLiteralAdmissibleTarget`
// predicate, the SAME `isCollectionLiteralEmptyAdmissibleTarget` carve-out
// (so an empty `[]` is still admitted at every collection target), the SAME
// form-vs-target split (a dict literal at a sequence-shaped field gets the
// form-specific diagnostic), and the SAME `mentionsUnsubstitutedTypeParam`
// deferral that #2926 added to checkAssignable (a field whose declared type
// mentions an unsubstituted generic parameter is checked at the
// instantiation, where the substituted target is concrete — judging the bare
// `U` here is the incompleteness its contract says not to judge, the same
// #1756 precedent the checkAssignable arm already encodes).
//
// ⚠ MAINTENANCE NOTE — the premise of the REJECT tests below can expire.
// They pin that a target is NOT on §15.4's list.  If a later revision ADDS
// one of these types to §15.4's target list, the corresponding test must be
// re-pointed at whatever remains outside the list — NOT deleted, and NOT
// quietly flipped to an accept: the assertion being pinned is "the closed
// list is enforced at the field-initializer position too", and it must keep
// failing loudly if it stops being.  The accept-controls in the other
// direction pin a distinctive NON-ZERO exit code (with the AOT 8-bit
// truncation acknowledged — see runAotOut for the observable) so "everything
// returned 0" cannot pass them.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// True when the compile was rejected AND the rejection is §15.4's generic
// "cannot target" diagnostic, naming the literal and the target type.
bool rejectedByRule15_4(const JitResult& r, const std::string& spelledTarget) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("collection literal `[...]` cannot target") !=
                std::string::npos &&
            d.message.find("`" + spelledTarget + "`") != std::string::npos &&
            d.message.find("§15.4") != std::string::npos)
            return true;
    }
    return false;
}

// True when the compile was rejected with the DICT-FORM-specific
// "dictionary literal `[k: v, ...]` cannot target" diagnostic — the
// remediation for the form-vs-target mismatch is "change the literal's
// form", not "change the target type", and the message must say so.
bool rejectedByDictFormRule(const JitResult& r, const std::string& spelledTarget) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("dictionary literal `[k: v, ...]` cannot target") !=
                std::string::npos &&
            d.message.find("`" + spelledTarget + "`") != std::string::npos &&
            d.message.find("§15.4") != std::string::npos)
            return true;
    }
    return false;
}

// True when the compile was rejected with the INTERFACE-specific diagnostic
// — the §15.4 closed-list rule has its own more specific message for the
// shape it names, mirroring the local-decl's interface arm (#1174 / #1866).
bool rejectedByInterfaceRule(const JitResult& r, const std::string& ifaceName) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("cannot target the interface type `" + ifaceName +
                           "`") != std::string::npos &&
            d.message.find("§15.4") != std::string::npos)
            return true;
    }
    return false;
}

// Compile a small program whose `H` class carries a single field `Slot` of
// `fieldType` initialized with `initializer`.  The returned program reads
// `h.Slot` only to force the class to be live — the test above this helper
// is the only thing inspecting diagnostics.
JitResult fieldOf(const std::string& fieldType,
                 const std::string& initializer) {
    return runAot(
        "module M; "
        "class H "
        "  public Slot: " + fieldType + " := " + initializer + "; "
        "  public constructor() end constructor; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    using var h: H := new H(); "
        "    return 0; "
        "  end Main; "
        "end C;");
}

JitResult propOf(const std::string& propType,
                 const std::string& initializer) {
    return runAot(
        "module M; "
        "class H "
        "  public property P: " + propType + " := " + initializer + "; "
        "  public constructor() end constructor; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    using var h: H := new H(); "
        "    return 0; "
        "  end Main; "
        "end C;");
}

}  // namespace

// =====================================================================
// #2958 — REJECT: every target outside §15.4's closed list, at the
// FIELD-INITIALIZER position.  Pre-fix each of these compiled CLEAN and
// degraded to a zero/null slot at runtime — a SILENTLY WRONG value
// behind zero user-facing diagnostics (the worst defect shape in the
// lane's defect catalog — see the run-tracker taxonomy).
// =====================================================================

BEZEL_TEST(sema_collection_literal_field_target_2958,
           object_target_rejected_spec_15_4) {
    // Pre-fix: compiled clean, `h.Slot is Object` read FALSE, exited 3.
    BEZEL_EXPECT_TRUE(
        rejectedByRule15_4(fieldOf("Object", "[1, 2, 3]"), "Object"));
}

BEZEL_TEST(sema_collection_literal_field_target_2958,
           primitive_int_target_rejected_spec_15_4) {
    // Pre-fix: compiled clean, exited 0 (the `int` slot was zero).
    BEZEL_EXPECT_TRUE(rejectedByRule15_4(fieldOf("int", "[1, 2, 3]"), "int"));
}

BEZEL_TEST(sema_collection_literal_field_target_2958,
           primitive_string_target_rejected_spec_15_4) {
    // Pre-fix: compiled clean, exited 1 (the empty string check passed).
    BEZEL_EXPECT_TRUE(
        rejectedByRule15_4(fieldOf("string", "[1, 2, 3]"), "string"));
}

BEZEL_TEST(sema_collection_literal_field_target_2958,
           plain_class_target_rejected_spec_15_4) {
    // A class that does NOT declare `FromCollectionLiteral` — §15.4's user
    // hook is opt-in, so a class without it is not a target.
    BEZEL_EXPECT_TRUE(rejectedByRule15_4(
        runAot("module M; "
               "class K public constructor() end constructor; end K; "
               "class H "
               "  public Slot: K := [1, 2, 3]; "
               "  public constructor() end constructor; "
               "end H; "
               "class C "
               "  public static method Main(): int "
               "    using var h: H := new H(); "
               "    return 0; "
               "  end Main; "
               "end C;"),
        "K"));
}

BEZEL_TEST(sema_collection_literal_field_target_2958,
           bare_type_parameter_target_rejected_spec_15_4) {
    // T is a bare type parameter inside a generic class. The field-init gate
    // intentionally does NOT defer on `mentionsUnsubstitutedTypeParam` for
    // the §15.4 half — the local-decl position has the same discipline
    // (#2914) — so the rejection fires at the class DECLARATION site, naming
    // the unsubstituted `T`. The trade-off (false-reject for `Box<Array<int>>`
    // where the substituted target would have been admissible) is the same
    // false-reject the local-decl position accepts.
    JitResult r = runAot(
        "module M; "
        "class Box<T> "
        "  public Slot: T := [1, 2, 3]; "
        "  public constructor() end constructor; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b: Box<int> := new Box<int>(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(rejectedByRule15_4(r, "T"));
}

// =====================================================================
// #2958 — REJECT: the form-vs-target split (#3031's contribution) at the
// FIELD-INITIALIZER position.  A dict literal at a sequence-shaped field
// gets the form-specific diagnostic — the remediation is to change the
// literal's FORM, not the target type.
// =====================================================================

BEZEL_TEST(sema_collection_literal_field_target_2958,
           dict_form_at_array_field_rejected_spec_15_4) {
    BEZEL_EXPECT_TRUE(rejectedByDictFormRule(
        fieldOf("Array<int>", "[\"a\": 1, \"b\": 2]"), "Array<int>"));
}

// =====================================================================
// #2958 — REJECT: the INTERFACE-specific diagnostic at the field-init
// position keeps its own, more specific message — mirroring the local-decl
// / return / call-arg arms (#1174 / #1866 / #3031).  The positive gate
// must NOT swallow it.
// =====================================================================

BEZEL_TEST(sema_collection_literal_field_target_2958,
           interface_target_keeps_its_specific_message_spec_15_4) {
    BEZEL_EXPECT_TRUE(
        rejectedByInterfaceRule(
            fieldOf("IEnumerable<int>", "[1, 2, 3]"), "IEnumerable"));
}

// =====================================================================
// #2958 — REJECT: every target outside §15.4's closed list, at the
// AUTO-PROPERTY-INITIALIZER position.  §23.1 makes the auto-property's
// backing field initializer run at the same construction-time step as an
// explicit field initializer, so the gate that #2914 added at local-decl
// and #3031 extended to return / call-arg / lambda-body has to apply here
// too.  Pre-fix the same silent-zero trap fired for these.
// =====================================================================

BEZEL_TEST(sema_collection_literal_field_target_2958,
           property_object_target_rejected_spec_15_4) {
    BEZEL_EXPECT_TRUE(
        rejectedByRule15_4(propOf("Object", "[1, 2, 3]"), "Object"));
}

BEZEL_TEST(sema_collection_literal_field_target_2958,
           property_primitive_target_rejected_spec_15_4) {
    BEZEL_EXPECT_TRUE(rejectedByRule15_4(propOf("int", "[1, 2, 3]"), "int"));
}

BEZEL_TEST(sema_collection_literal_field_target_2958,
           property_interface_target_keeps_specific_message_spec_15_4) {
    BEZEL_EXPECT_TRUE(rejectedByInterfaceRule(
        propOf("IEnumerable<int>", "[1, 2, 3]"), "IEnumerable"));
}

// =====================================================================
// #2958 — ACCEPT-CONTROLS: §15.4's list is enforced at the field-init
// position, NOT narrowed.  Every admissible target still materializes AND
// still carries its values.  The expected value is deliberately distinctive
// (e.g. 321 = 1 + 2*10 + 3*100) and observed via stdout (runAotOut) so the
// 8-bit AOT exit-code truncation (#2514 / `runAot`'s second semantic
// difference) cannot mask a degraded value.
// =====================================================================

BEZEL_TEST(sema_collection_literal_field_target_2958,
           inline_array_field_still_materializes_spec_15_4) {
    BEZEL_EXPECT_EQ(
        runAotOut("module M; "
                  "class H "
                  "  public Slot: [3]int := [1, 2, 3]; "
                  "  public constructor() end constructor; "
                  "end H; "
                  "class C "
                  "  public static method Main(): int "
                  "    using var h: H := new H(); "
                  "    Console.WriteLine(h.Slot[0] + h.Slot[1] * 10 "
                  "                      + h.Slot[2] * 100); "
                  "    return 0; "
                  "  end Main; "
                  "end C;"),
        std::string("321"));
}

BEZEL_TEST(sema_collection_literal_field_target_2958,
           heap_array_field_still_materializes_spec_15_4) {
    BEZEL_EXPECT_EQ(
        runAotOut("module M; "
                  "class H "
                  "  public Slot: Array<int> := [1, 2, 3]; "
                  "  public constructor() end constructor; "
                  "end H; "
                  "class C "
                  "  public static method Main(): int "
                  "    using var h: H := new H(); "
                  "    Console.WriteLine(h.Slot[0] + h.Slot[1] * 10 "
                  "                      + h.Slot[2] * 100); "
                  "    return 0; "
                  "  end Main; "
                  "end C;"),
        std::string("321"));
}

BEZEL_TEST(sema_collection_literal_field_target_2958,
           empty_literal_at_array_field_admitted_spec_15_4) {
    // §15.4's empty-literal carve-out: `[]` is admitted at every §15.4
    // collection target (the target type decides between empty seq and
    // empty dict).  The gate's empty branch (`isCollectionLiteralEmptyAdmissibleTarget`)
    // must NOT swallow this.
    BEZEL_EXPECT_EQ(
        runAotOut("module M; "
                  "class H "
                  "  public Slot: Array<int> := []; "
                  "  public constructor() end constructor; "
                  "end H; "
                  "class C "
                  "  public static method Main(): int "
                  "    using var h: H := new H(); "
                  "    Console.WriteLine(h.Slot.Count); "
                  "    return 0; "
                  "  end Main; "
                  "end C;"),
        std::string("0"));
}

BEZEL_TEST(sema_collection_literal_field_target_2958,
           empty_literal_at_dict_field_admitted_spec_15_4) {
    // The empty-literal carve-out covers the dict family too: `[]` at a
    // `Dictionary<K,V>` is the empty dictionary.
    BEZEL_EXPECT_EQ(
        runAotOut("module M; "
                  "class H "
                  "  public Slot: Dictionary<string, int> := []; "
                  "  public constructor() end constructor; "
                  "end H; "
                  "class C "
                  "  public static method Main(): int "
                  "    using var h: H := new H(); "
                  "    Console.WriteLine(h.Slot.Count); "
                  "    return 0; "
                  "  end Main; "
                  "end C;"),
        std::string("0"));
}

BEZEL_TEST(sema_collection_literal_field_target_2958,
           inline_array_property_still_materializes_spec_15_4) {
    BEZEL_EXPECT_EQ(
        runAotOut("module M; "
                  "class H "
                  "  public property P: [3]int := [1, 2, 3]; "
                  "  public constructor() end constructor; "
                  "end H; "
                  "class C "
                  "  public static method Main(): int "
                  "    using var h: H := new H(); "
                  "    Console.WriteLine(h.P[0] + h.P[1] * 10 "
                  "                      + h.P[2] * 100); "
                  "    return 0; "
                  "  end Main; "
                  "end C;"),
        std::string("321"));
}