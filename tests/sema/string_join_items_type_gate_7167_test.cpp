// fix #7167 — stdlib §9.6 declares `string.Join`'s ITEMS parameter as an
// `IEnumerable<string>`, but the compiler never checked it: an ill-typed
// items argument was ADMITTED, reached the lowering, and was materialised as
// 8-byte pointer elements.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   stdlib §9.6 (v0.345), verbatim:
//     "public static method Join(separator: string, items: IEnumerable<string>): string;"
//
//   ⇒⇒ `items` takes an `IEnumerable<string>`. Every element of the sequence
//   is therefore a `string` — one pointer, eight bytes.
//
//   stdlib §4.4 / §2.x (v0.345), verbatim:
//     "public class Array<T>: IList<T>, ICollectionLiteralTarget<Array<T>, T>"
//     "public interface IList<T>: IReadOnlyList<T>, ICollection<T>"
//     "public interface ICollection<T>: IReadOnlyCollection<T>"
//     "public interface IReadOnlyCollection<out T>: IEnumerable<T>"
//     "public interface IEnumerable<out T>"
//
//   ⇒⇒ AN `Array<string>` IS an `IEnumerable<string>` (the chain above ends
//   at `IEnumerable<T>`), and `IEnumerable<T>` is COVARIANT (`out T`).
//
//   stdlib §9.6 (v0.345), verbatim:
//     "public class String: IEnumerable<char>, IComparable<string>, IEquatable<string>"
//
//   ⇒⇒ A `string` is an `IEnumerable<char>` — NOT an `IEnumerable<string>`.
//   So a bare `string` in the items slot is not an admissible argument
//   either, even though it is "a sequence of things".
//
// THE DEFECT.
//
//   stdlib §9.6's SEPARATOR was type-gated (#7150), but the ITEMS slot was
//   not: the gate in `src/sema/ExprTypecheck_Calls.cpp` bound only the
//   `separator` parameter. So the call was HALF-gated, and an ill-typed
//   items argument reached the lowering.
//
//   MEASURED pre-fix on the base of this branch (9961f766), each shape
//   compiled with the AOT path and run:
//
//     string.Join(",", Array<int>)   →  COMPILED CLEANLY and printed the
//       empty string. The items were materialised through the shared lazy
//       path and drained with a HARDCODED 8-byte element width, so the drain
//       copied 8 bytes out of a 4-byte element slot — the ASan
//       heap-buffer-overflow READ #7167 measured, and, without the
//       sanitizer, a silent empty result.
//
//     string.Join(",", "ab")         →  COMPILED CLEANLY. A `string`'s
//       elements are `char`s (4 bytes), so the same 8-byte drain read past
//       the end of the element buffer, and the value was interpreted as a
//       `bz_string` header that is not one.
//
//   ⇒⇒ THE COMPILER MUST REJECT BOTH. The gate belongs in the front end:
//   §5.1's implicit-conversion table has no row that admits an `Array<int>`
//   (or a `string`) to an `IEnumerable<string>`, and `IEnumerable<T>`'s
//   covariance (`out T`) does not help: `int` is not a supertype of
//   `string`.
//
// THE ORACLE.
//
//   A SILENT-ACCEPTANCE defect is invisible in the exit code: the illegal
//   program compiled and ran. So the oracle is the DIAGNOSTIC, and it must
//   be a REAL front-end rejection at a REAL location — not a crash, not a
//   hang, not an IR-verification failure.
//
//   The diagnostic must name the parameter AND the type it must have, so
//   each row asserts the full message, not merely "some error":
//
//     "the items argument to `string.Join` (expected `IEnumerable<string>`)
//      has a type that is not implicitly convertible to the target type (§5.1)"
//
//   And the CONTROL matters as much as the rejects: a gate that rejected
//   the LEGAL call would be a silent-acceptance inversion in the other
//   direction and would be exactly as wrong. So the controls assert the
//   JOINED VALUE, byte for byte — `string.Join(",", Array<string>)` must
//   still print "a,b", and a LAZY `IEnumerable<string>` (the §9.6 shape, not
//   the concrete `Array<string>`) must still print "ab,cd".

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// A bare `module M; class C ... end C;` shell — the string members are
// compiler intrinsics, so no imports or hierarchy are required.
std::string prog(const std::string& mainBody) {
    return "module M;\nclass C\n  public static method Main(): int\n" +
           mainBody + "\n  end Main;\nend C;\n";
}

// The §5.1 wording for the ITEMS parameter, with the declared type named.
const char* kItemsNotConvertible =
    "the items argument to `string.Join` (expected `IEnumerable<string>`) "
    "has a type that is not implicitly convertible to the target type (§5.1)";

// The §7 nullability wording for the same parameter.
const char* kItemsNullable =
    "the items argument to `string.Join` (expected `IEnumerable<string>`) "
    "is nullable but the target type is non-nullable (§7)";

// True iff SOME error diagnostic carries the given text.
bool errorWith(const JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// True iff the diagnostic carries the §5.1 wording for the items parameter.
bool itemsNotConvertible(const JitResult& r) {
    return errorWith(r, kItemsNotConvertible);
}

// True iff the diagnostic carries the §7 wording for the items parameter.
bool itemsNullable(const JitResult& r) {
    return errorWith(r, kItemsNullable);
}

// The rejection must be a REAL front-end rejection at a real location — not
// a crash, not a hang, not an IR-verification failure. (A compiler that
// dies is the defect this issue is about.)
bool rejectedAtARealLocation(const JitResult& r) {
    return bezel::test::failedAtCompileTimeWithRealLocation(r);
}

}  // namespace

// ── REJECT — an `Array<int>` in the items slot ────────────────────────────
//
// MEASURED pre-fix: the program COMPILED CLEANLY and printed the empty
// string (and tripped an ASan heap-buffer-overflow READ of size 8 when the
// generated module was sanitized).
//
// `IEnumerable<T>` is covariant (`out T`), so an `IEnumerable<int>` would be
// an `IEnumerable<string>` only if `int` were a supertype of `string` — it
// is not (§12.9). The `Array<int>` is not an `IEnumerable<string>`.
BEZEL_TEST(sema_string_join_items_type_gate_7167,
           array_int_items_rejected_9_6) {
    auto r = runAot(prog(
        "    using var nums := new Array<int>();\n"
        "    nums.Add(1);\n"
        "    nums.Add(2);\n"
        "    Console.WriteLine(string.Join(\",\", nums));\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(itemsNotConvertible(r));
    BEZEL_EXPECT_TRUE(rejectedAtARealLocation(r));
}

// ── REJECT — a bare `string` in the items slot ────────────────────────────
//
// MEASURED pre-fix: the program COMPILED CLEANLY (the same ASan overflow,
// with the 4-byte `char` element slot as the over-read region).
//
// A `string` is an `IEnumerable<char>` (§9.6's own declaration), never an
// `IEnumerable<string>`, so it is not an admissible items argument.
BEZEL_TEST(sema_string_join_items_type_gate_7167,
           plain_string_items_rejected_9_6) {
    auto r = runAot(prog(
        "    Console.WriteLine(string.Join(\",\", \"ab\"));\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(itemsNotConvertible(r));
    BEZEL_EXPECT_TRUE(rejectedAtARealLocation(r));
}

// ── REJECT — an `int` in the items slot ───────────────────────────────────
//
// A scalar is not a sequence at all; §5.1 has no conversion from `int` to
// an `IEnumerable<string>`.
BEZEL_TEST(sema_string_join_items_type_gate_7167,
           int_items_rejected_9_6) {
    auto r = runAot(prog(
        "    Console.WriteLine(string.Join(\",\", 5));\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(itemsNotConvertible(r));
    BEZEL_EXPECT_TRUE(rejectedAtARealLocation(r));
}

// ── REJECT — an `Array<Object>` in the items slot ─────────────────────────
//
// `Object` is the root type, but `IEnumerable<T>`'s covariance runs the
// other way: an `IEnumerable<Object>` is not an `IEnumerable<string>`
// (§12.9 — covariance widens the element type, it does not narrow it).
BEZEL_TEST(sema_string_join_items_type_gate_7167,
           array_object_items_rejected_9_6) {
    auto r = runAot(prog(
        "    using var objs := new Array<Object>();\n"
        "    objs.Add(\"a\");\n"
        "    Console.WriteLine(string.Join(\",\", objs));\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(itemsNotConvertible(r));
    BEZEL_EXPECT_TRUE(rejectedAtARealLocation(r));
}

// ── REJECT — a `null` items argument (the §7 rule) ────────────────────────
//
// `null` is nullable and the declared parameter type is non-nullable, so
// §7's rule rejects it. This row pins that the gate reaches the §7 rule
// and not merely the §5.1 one — a gate that only checked convertibility
// would let a `null` through, and the lowering would then read a null
// string header.
BEZEL_TEST(sema_string_join_items_type_gate_7167,
           null_items_rejected_7) {
    auto r = runAot(prog(
        "    Console.WriteLine(string.Join(\",\", null));\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(itemsNullable(r));
    BEZEL_EXPECT_TRUE(rejectedAtARealLocation(r));
}

// ── REJECT — the REORDERED NAMED-ARGUMENT spelling ───────────────────────
//
// §22.5: a named argument binds by NAME, not by position. So
// `string.Join(items := ..., separator := ...)` supplies the FIRST written
// argument to the `items` parameter. The gate must bind by name too — a
// gate that read the items slot POSITIONALLY would check the separator
// twice and leave the items slot unchecked.
//
// The row uses an `Array<int>` in the `items` slot, so the gate must fire
// on the items slot under the reordered spelling.
BEZEL_TEST(sema_string_join_items_type_gate_7167,
           reordered_named_args_bind_the_items_slot_by_name_22_5) {
    auto r = runAot(prog(
        "    using var nums := new Array<int>();\n"
        "    nums.Add(1);\n"
        "    Console.WriteLine(string.Join(items := nums, separator := \",\"));\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(itemsNotConvertible(r));
    BEZEL_EXPECT_TRUE(rejectedAtARealLocation(r));
}

// ── CONTROL — an `Array<string>` in the items slot still JOINS ───────────
//
// §9.6's declared shape. The joined value is asserted byte for byte: a
// gate that rejected the legal call would fail this row, and a gate that
// let the call through but lowered it wrongly would also fail it.
//
// (The value is the strongest oracle here: the pre-fix defect produced an
// EMPTY string for the illegal shapes, and a silently-empty join for a
// legal one would be indistinguishable from a clean exit.)
BEZEL_TEST(sema_string_join_items_type_gate_7167,
           control_array_string_items_still_join) {
    const std::string src = prog(
        "    using var parts := new Array<string>();\n"
        "    parts.Add(\"a\");\n"
        "    parts.Add(\"b\");\n"
        "    Console.WriteLine(string.Join(\",\", parts));\n"
        "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "a,b");
}

// ── CONTROL — the SEPARATOR gate is undisturbed ───────────────────────────
//
// #7150 gated the separator. A fix here must not have weakened it, so the
// row asserts the separator's own diagnostic is still produced.
BEZEL_TEST(sema_string_join_items_type_gate_7167,
           control_separator_gate_undisturbed_7150) {
    auto r = runAot(prog(
        "    using var parts := new Array<string>();\n"
        "    parts.Add(\"a\");\n"
        "    Console.WriteLine(string.Join(',', parts));\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(errorWith(
        r, "the separator argument to `string.Join` has a type that is not "
           "implicitly convertible to the target type (§5.1)"));
    BEZEL_EXPECT_TRUE(rejectedAtARealLocation(r));
}

// ── CONTROL — a LAZY `IEnumerable<string>` still joins ────────────────────
//
// §9.6 declares `items: IEnumerable<string>`, NOT a concrete
// `Array<string>`: the lazy-source lowering (#1182) must still work behind
// the gate. This is the row that catches a gate that keyed on the CONCRETE
// type instead of the declared interface — the exact silent-acceptance
// inversion the other direction.
BEZEL_TEST(sema_string_join_items_type_gate_7167,
           control_lazy_query_items_still_join) {
    const std::string src =
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var xs := new Array<string>(); "
        "  xs.Add(\"ab\"); xs.Add(\"cd\"); "
        "  var s := string.Join(\",\", xs.Where(lambda(x: string) => "
        "x.Length > 0)); "
        "  Console.WriteLine(s); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "ab,cd");
}

// ── CONTROL — a value TYPED as `IEnumerable<string>` still joins ──────────
//
// The declared-interface spelling, spelled out at the declaration site:
// the gate must accept it (the variable's type IS `IEnumerable<string>`),
// not merely accept an `Array<string>` by its concrete type.
BEZEL_TEST(sema_string_join_items_type_gate_7167,
           control_declared_ienumerable_string_items_still_join) {
    const std::string src = prog(
        "    using var parts := new Array<string>();\n"
        "    parts.Add(\"a\");\n"
        "    parts.Add(\"b\");\n"
        "    var seq: IEnumerable<string> := parts;\n"
        "    Console.WriteLine(string.Join(\",\", seq));\n"
        "    return 0;");
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(runAotOut(src), "a,b");
}

// ── CONTROL — the ARITY diagnostic is undisturbed ─────────────────────────
//
// #7140's arity gate keys on the argument COUNT; a fix here must not have
// disturbed it, and must not have produced a convertibility diagnostic for
// an arity error (which would be a misleading second message).
BEZEL_TEST(sema_string_join_items_type_gate_7167,
           control_arity_gate_undisturbed) {
    auto r = runAot(prog(
        "    using var parts := new Array<string>();\n"
        "    parts.Add(\"a\");\n"
        "    Console.WriteLine(string.Join(\",\", parts, parts));\n"
        "    return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(errorWith(
        r, "the call to `string.Join` supplies"));
    BEZEL_EXPECT_FALSE(itemsNotConvertible(r));
}
