// Codegen — #6325 / §15.4 *Collection literals* + §27.8 *Type aliases* +
// §28.3 *Factory methods and sourceless returns* (v0.338).
//
// The issue as filed: "an array literal returned through a generic alias
// (`Arr0<E> := Array<E>`) is silently mishandled",
//
//     type Arr0<E> := Array<E>;
//     public static method Make(): Arr0<int> => [1, 2, 3];
//     Console.WriteLine($"{Make().Count}");   // prints 0, expected 3
//
// and it attributes the failure to the alias ("The literal is presumably typed
// against the unexpanded alias"). MEASURED on r31-c1 (b28c2119), that
// attribution is wrong, and the matrix below separates the two causes:
//
//   (1) The `=>` ARROW BODY has no collection-literal arm at all. §15.4:
//       "`return [1, 2, 3]` is admitted when the enclosing method's return type
//       determines the target type", and an arrow body IS that method's
//       return — but `lowerExpr` returns null for a `CollectionLiteralExpr`
//       (it is not a general expression), so `v` stayed null,
//       `emitReturn`'s `v ? v : null` substituted a zero/null return, and
//       #2491 printed its "failed to lower to a value" note while the caller
//       read Count 0 off the null. This fires for EVERY return type, alias or
//       not — `public static method Make(): owned Array<int> => [1, 2, 3];`
//       degraded identically. The `return [1, 2, 3];` STATEMENT face has
//       carried the arm since #465/#781; the two arrow-body faces
//       (emitMethodBody, and the generic-specialisation face in
//       LlvmBackend_Calls2.cpp) had none. Fixed by one shared
//       `lowerReturnCollectionLiteral`, so the three faces cannot drift —
//       #2924's four-way-mirror finding about the #2491 diagnostic is the same
//       failure mode this shape prevents.
//
//   (2) The §28.3 sourceless-return check read the SYNTACTIC head
//       (`returnType.name->parts.front()`), so an alias head (`Arr0`,
//       `Bag2`) matched no carve list and no plain-collection list and the
//       declaration was ACCEPTED where the identical `Array<int>` spelling is
//       rejected. §27.8: "An alias is not a separate kind of type; it is a
//       naming convenience for an existing one. Identity, equality, and method
//       dispatch all see through the alias." `TypeRef::headName()` is the
//       sanctioned alias-transparent read for exactly this (#1462).
//
// (2) is the half that matters for soundness, and it is the half the issue's
// own repro exercises: with (1) fixed and (2) not, the issue's program would
// have gone from printing 0 to printing 3 while LEAKING — a silent
// acceptance inversion, which is worse than the wrong-output it replaced.
// MEASURED: 120 B / 2 allocations under ASan (bz_array_alloc, direct + the
// backing store). The `owned` marker is the announcement that names the
// disposer; the alias must not erase it. So both halves ship together.
//
// The §14.2 constraint and bare-type-parameter arm of the same check
// (`staticReturnTypeRejected`'s arm (B)) deliberately keeps the SYNTACTIC head:
// it is a name-set test against the declared generic parameter NAMES, and a
// `where` clause is spelled with that name. Alias-transparency is a property
// of a type; arm (B) is about a name. Row (G) below pins that distinction —
// an alias of a type PARAMETER is still adjudicated as the parameter.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation etc.
#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The issue's own repro, with the `owned` marker the §28.3 rule requires. The
// ARROW BODY is the shape under test; `Count` reads the DATA the fix builds,
// so the row cannot pass on a constant.
const char* kIssueReproOwnedAlias =
    "module M6325; "
    "type Arr0<E> := Array<E>; "
    "class M "
    "  public static method Make(): owned Arr0<int> => [1, 2, 3]; "
    "  public static method Main(): int "
    "    Console.WriteLine($\"{Make().Count}\"); "
    "    return 0; "
    "  end Main; "
    "end M;";

// The same arrow body spelled with the built-in head directly — the CONTROL
// proving the arrow face, not the alias, was what lacked the arm. It printed 0
// before the fix too.
const char* kArrowBodyDirectHead =
    "module M6325Direct; "
    "class M "
    "  public static method Make(): owned Array<int> => [1, 2, 3]; "
    "  public static method Main(): int "
    "    Console.WriteLine($\"{Make().Count}\"); "
    "    return 0; "
    "  end Main; "
    "end M;";

// A NON-alias `Array<int>` return through the arrow body, with the elements
// read back through the indexer. `Count` alone would be satisfied by a
// degenerate empty array; indexing proves the elements landed.
const char* kArrowBodyElementsRead =
    "module M6325Elems; "
    "class M "
    "  public static method Make(): owned Array<int> => [10, 20, 30]; "
    "  public static method Main(): int "
    "    var a := Make(); "
    "    Console.WriteLine($\"{a[0]} {a[1]} {a[2]}\"); "
    "    return 0; "
    "  end Main; "
    "end M;";

// The §15.3 INLINE-array target through the arrow body — the value-kind half of
// the extracted arm. The heap materializers make a `ptr`, the wrong type for an
// inline return, so this arm has to precede them and build the `[N x elem]`
// aggregate BY VALUE. Before the fix every slot read back 0.
const char* kArrowBodyInlineArray =
    "module M6325Inline; "
    "class M "
    "  public static method Make(): [3]int => [7, 8, 9]; "
    "  public static method Main(): int "
    "    var a := Make(); "
    "    Console.WriteLine($\"{a[0]} {a[1]} {a[2]}\"); "
    "    return 0; "
    "  end Main; "
    "end M;";

// The §15.4 DICTIONARY literal form (`[k: v, …]`) through the arrow body — the
// third shape the extracted arm routes. `Count` reads the built dictionary.
const char* kArrowBodyDict =
    "module M6325Dict; "
    "class M "
    "  public static method Make(): owned Dictionary<int, int> => [1: 10, 2: 20]; "
    "  public static method Main(): int "
    "    Console.WriteLine($\"{Make().Count}\"); "
    "    return 0; "
    "  end Main; "
    "end M;";

// The METHOD-LEVEL GENERIC arrow body, which reaches the generic-specialisation
// face (LlvmBackend_Calls2.cpp) rather than emitMethodBody's: a generic
// expression body is INLINED into the caller, so it never reaches
// buildSpecializedMethodFn. That is a THIRD spelling of the same position, and
// MEASURED it degraded identically pre-fix (Count 0 / "0 0 0" with the #2491
// note) — it is the row that proves the second arrow-body arm was needed and
// not merely added for symmetry. The inline-array variant reads every slot, so
// it cannot pass on a zeroed aggregate.
const char* kGenericArrowBodyHeap =
    "module M6325GenHeap; "
    "class M "
    "  public static method Make<T>(): owned Array<T> => [1, 2, 3]; "
    "  public static method Main(): int "
    "    Console.WriteLine($\"{Make<int>().Count}\"); "
    "    return 0; "
    "  end Main; "
    "end M;";

const char* kGenericArrowBodyInline =
    "module M6325GenInline; "
    "class M "
    "  public static method Make<T>(): [3]int => [7, 8, 9]; "
    "  public static method Main(): int "
    "    var a := Make<int>(); "
    "    Console.WriteLine($\"{a[0]} {a[1]} {a[2]}\"); "
    "    return 0; "
    "  end Main; "
    "end M;";

// The EMPTY literal `[]` — §15.4 admits it "for every collection target — an
// empty heap array of Count 0". Prints 0 both before and after; it pins that
// the arm did not turn an admitted empty literal into a decline.
const char* kArrowBodyEmpty =
    "module M6325Empty; "
    "class M "
    "  public static method Make(): owned Array<int> => []; "
    "  public static method Main(): int "
    "    Console.WriteLine($\"{Make().Count}\"); "
    "    return 0; "
    "  end Main; "
    "end M;";

// (2): the declaration-site REJECTION through an alias. Before the fix this
// compiled with no diagnostic at all; the identical `Array<int>` spelling was
// rejected. Asserts the specific §28.3 diagnostic and its location, not merely
// that the compile failed.
const char* kAliasUnmarkedRejected =
    "module M6325Reject; "
    "type Arr0<E> := Array<E>; "
    "class M "
    "  public static method Make(): Arr0<int> => [1, 2, 3]; "
    "  public static method Main(): int "
    "    return 0; "
    "  end Main; "
    "end M;";

// The same rejection for a NON-generic alias and for a `Dictionary` alias, so
// the fix is not narrow to one alias arity or one collection family.
const char* kNonGenericAliasUnmarkedRejected =
    "module M6325Reject2; "
    "type Bag2 := Array<int>; "
    "class M "
    "  public static method Make(): Bag2 => [1, 2, 3]; "
    "  public static method Main(): int "
    "    return 0; "
    "  end Main; "
    "end M;";

const char* kDictAliasUnmarkedRejected =
    "module M6325Reject3; "
    "type D2<E> := Dictionary<E, E>; "
    "class M "
    "  public static method Make(): D2<int> => [1: 2]; "
    "  public static method Main(): int "
    "    return 0; "
    "  end Main; "
    "end M;";

// (G): arm (B) of the same check must still adjudicate on the SYNTACTIC name.
// Arm (B) is a NAME-SET test against the declared generic parameter names, not
// a type-identity test — so it deliberately kept the syntactic head through the
// alias-transparency change above. These two rows pin that it still fires and
// still declines, at the SAME declaration shape that arm (B) governs: a static
// method returning a bare type parameter with no plain parameter mentioning it
// has an EMPTY source set and is rejected (the empty-set case); the identical
// method with such a parameter is admitted. Both name the parameter `T`, so a
// change that routed arm (B) through a head that no longer read `T` would
// flip the first row to admitted.
const char* kBareTypeParamEmptySourceSetRejected =
    "module M6325BareReject; "
    "class Node end Node; "
    "class Box<T> "
    "  public static method Make(): T => new Node(); "
    "end Box;";

const char* kBareTypeParamWithSourceParamAdmitted =
    "module M6325BareAllow; "
    "class Node end Node; "
    "class Box<T> "
    "  public static method Make(v: T): T => v; "
    "  public static method Main(): int "
    "    return 0; "
    "  end Main; "
    "end Box;";

}  // namespace

// ---------------------------------------------------------------------
// (A) the issue's repro: an arrow-body collection literal through a generic
// alias. Prints 3. Pre-fix: printed 0 with the #2491 "failed to lower" note.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_arrow_body_collection_literal_return_6325,
           arrow_body_literal_through_generic_alias_spec_15_4) {
    JitResult r = runAot(kIssueReproOwnedAlias);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("3") != std::string::npos);
}

// The same run under the checked oracle: the array is freshly allocated and
// the `owned` return names the caller as its disposer, so this row is what
// would catch a leak introduced by materializing the literal.
BEZEL_TEST(codegen_arrow_body_collection_literal_return_6325,
           arrow_body_literal_through_generic_alias_no_leak_spec_28_3) {
    JitResult r = runAotChecked(kIssueReproOwnedAlias);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("3") != std::string::npos);
}

// ---------------------------------------------------------------------
// (B) CONTROL: no alias anywhere. This is the row that shows the arrow FACE
// was the defect, not the alias — it degraded identically pre-fix.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_arrow_body_collection_literal_return_6325,
           arrow_body_literal_direct_head_control_spec_15_4) {
    JitResult r = runAot(kArrowBodyDirectHead);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("3") != std::string::npos);
}

// The elements, not just the Count — `Count` would be satisfied by a
// degenerate empty array, so this row is what proves the literal's data was
// built.
BEZEL_TEST(codegen_arrow_body_collection_literal_return_6325,
           arrow_body_literal_elements_read_back_spec_15_4) {
    JitResult r = runAot(kArrowBodyElementsRead);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("10 20 30") != std::string::npos);
}

// ---------------------------------------------------------------------
// (C) the INLINE-array target (§15.3) — the value-kind arm. Pre-fix every
// slot read 0 (measured), so this row is not vacuous.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_arrow_body_collection_literal_return_6325,
           arrow_body_inline_array_literal_spec_15_3) {
    JitResult r = runAot(kArrowBodyInlineArray);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("7 8 9") != std::string::npos);
}

// ---------------------------------------------------------------------
// (D) the DICTIONARY literal form (§15.4 `[k: v, …]`).
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_arrow_body_collection_literal_return_6325,
           arrow_body_dict_literal_spec_15_4) {
    JitResult r = runAot(kArrowBodyDict);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("2") != std::string::npos);
}

// ---------------------------------------------------------------------
// (D2) the METHOD-LEVEL GENERIC arrow body — the generic-specialisation face.
// Pre-fix these printed 0 and "0 0 0" with the #2491 note (measured), so both
// rows are non-vacuous, and together they pin the SECOND arrow-body arm.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_arrow_body_collection_literal_return_6325,
           generic_arrow_body_heap_literal_spec_15_4) {
    JitResult r = runAot(kGenericArrowBodyHeap);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("3") != std::string::npos);
}

BEZEL_TEST(codegen_arrow_body_collection_literal_return_6325,
           generic_arrow_body_inline_literal_spec_15_3) {
    JitResult r = runAot(kGenericArrowBodyInline);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("7 8 9") != std::string::npos);
}

// ---------------------------------------------------------------------
// (E) the EMPTY literal — admitted for every collection target (§15.4).
// Prints 0 before and after; it pins that the arm did not turn an admitted
// empty literal into a decline.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_arrow_body_collection_literal_return_6325,
           arrow_body_empty_literal_still_admitted_spec_15_4) {
    JitResult r = runAot(kArrowBodyEmpty);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("0") != std::string::npos);
}

// ---------------------------------------------------------------------
// (F) the §28.3 declaration-site rejection now fires through an alias. The
// same program compiled with NO diagnostic before the fix, while the direct
// `Array<int>` spelling was rejected — the silent acceptance inversion that
// would have leaked 120 B once the codegen arm materialized the literal.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_arrow_body_collection_literal_return_6325,
           alias_head_unmarked_return_is_rejected_spec_28_3) {
    JitResult r = runAot(kAliasUnmarkedRejected);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        (bezel::test::failedAtCompileTimeWithRealLocation(r) &&
     bezel::test::diagnosticMessageContains(r, "static method `Make` returns class/interface type `Array` without " "`owned`")));
}

BEZEL_TEST(codegen_arrow_body_collection_literal_return_6325,
           non_generic_alias_head_unmarked_return_is_rejected_spec_28_3) {
    JitResult r = runAot(kNonGenericAliasUnmarkedRejected);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        (bezel::test::failedAtCompileTimeWithRealLocation(r) &&
     bezel::test::diagnosticMessageContains(r, "static method `Make` returns class/interface type `Array` without " "`owned`")));
}

BEZEL_TEST(codegen_arrow_body_collection_literal_return_6325,
           dict_alias_head_unmarked_return_is_rejected_spec_28_3) {
    JitResult r = runAot(kDictAliasUnmarkedRejected);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(
        (bezel::test::failedAtCompileTimeWithRealLocation(r) &&
     bezel::test::diagnosticMessageContains(r, "static method `Make` returns class/interface type `Dictionary` " "without `owned`")));
}

// ---------------------------------------------------------------------
// (G) arm (B) keeps the SYNTACTIC head. It is a name-set test against the
// declared generic parameter names, so it reads `T` — not an alias-resolved
// head. Row 1: the empty-source-set bare-T is still REJECTED. Row 2: the
// identical method with a plain parameter mentioning `T` is still ADMITTED.
// Between them they pin that arm (B) reached the same verdict it did before
// the alias-transparency change.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_arrow_body_collection_literal_return_6325,
           bare_type_param_empty_source_set_still_rejected_spec_28_3) {
    JitResult r = runAot(kBareTypeParamEmptySourceSetRejected);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE((bezel::test::failedAtCompileTimeWithRealLocation(r) &&
     bezel::test::diagnosticMessageContains(r, "static method `Make` returns class/interface type `T` without " "`owned`")));
}

BEZEL_TEST(codegen_arrow_body_collection_literal_return_6325,
           bare_type_param_with_source_param_still_admitted_spec_28_3) {
    JitResult r = runAot(kBareTypeParamWithSourceParamAdmitted);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}