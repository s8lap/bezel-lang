// fix #7064 (§23.4 / §5.1 / stdlib §4.4, v0.343): an INDEXER'S INDEX ARGUMENT
// IS NOT TYPE-CHECKED, so a non-`int` index reaches codegen as malformed IR.
//
// THE SPEC.
//
//   * §23.4: "An indexer is a property whose name is replaced by
//     `[parameters]`" — an index argument is therefore a PARAMETER PASS, and
//     §23.4 makes the parameter TYPE LIST what admits an index ("Indexers may
//     be overloaded by parameter type list").
//   * stdlib §4.4 declares `Array<T>`'s indexer as `public property [i: int]:
//     T`, and every other int-indexed builtin declares the same `int`
//     parameter — the list interfaces `IReadOnlyList<T>` (§4.1), `IList<T>`
//     (§4.1) and `IOwningList<T>` (§4.2), `OwningArray<T>` (§4.5), the
//     buffer/span family (§4.14), the array-backed spans (§4.15),
//     `FrozenArray<T>` (§4.19) and `ReadOnlyStringSpan` (§3.3).
//   * §5.1 governs the conversion at that parameter pass, and its table is
//     CLOSED and lists only numeric WIDENING rows: `sbyte`/`byte`/`short`/
//     `ushort`/`char` -> `int` are rows, `int` -> `int` is the identity, and
//     `uint`/`long`/`ulong`/`float`/`double`/`decimal`/`bool`/`string`/a
//     record/a class/an enum -> `int` has NO row. §5.1's "Between integers and
//     `bool`" bullet makes the `bool` case explicit, and the "never implicit"
//     list's signed/unsigned bullet makes the `uint` case explicit.
//
// WHAT THE PRE-FIX COMPILER DID. The index argument's type was never
// consulted at the index position. MEASURED on the pre-fix base (c74fb5b6)
// over `Array<int>`:
//
//   a[1.5]  (double) -> "SExt only operates on integer" / `sext double …`
//   a[s]    (string) -> "SExt only operates on integer" / `sext ptr …`
//   a[r]    (record) -> "SExt only operates on integer" / `sext %record.X …`
//   a[o]    (class)  -> "SExt only operates on integer" / `sext ptr …`
//   a[j]    (long)   -> COMPILED AND RAN
//   a[b]    (bool)   -> COMPILED AND RAN
//
// Four of the six are an INTERNAL COMPILER ERROR where §5.1 asks for a clean
// diagnostic, and the two that compiled are §5.1 errors too — the issue's own
// expected-behaviour section says so ("§5.1's table is closed and lists only
// numeric widening rows — so `a[<a double>]`, `a[<a string>]`, `a[<a
// record>]` and `a[<a long>]` are all §5.1 errors"). The `long` and `bool`
// rows are therefore BEHAVIOUR FLIPS: programs that compiled and ran before
// this fix are rejected now, and the commit body names them.
//
// The same hole was open on the span family, the list interfaces, `OwningArray`
// / `FrozenArray`, `string` and inline arrays — the issue's title names only
// the collection families, but the index POSITION is the same one in all of
// them, and every one of them reached codegen the same way.
//
// It was open on a USER-declared indexer too, and there the outcome was worse
// than a diagnostic: `property [i: int]: int => i * 2` indexed with a `double`
// compiled and PRINTED GARBAGE (MEASURED: 972021523), because the argument
// reached the accessor with no conversion at all.
//
// THE ACCEPT CONTROLS ARE PART OF THE CONTRACT. §5.1 admits `char`/`byte`/
// `short` -> `int` (table rows), admits an integer COMPILE-TIME CONSTANT of
// any integer type whose value fits the target (§5.1's constant-narrowing
// clause), and §15.6's RANGE slicing form (`arr[0..1]`, `arr[r]` at a
// `Range<int>` value) is a slice and not an element index at all. Those must
// keep compiling; the rows below pin them.
//
// The DICTIONARY family is not this fix's business: its index parameter is the
// key type `K`, not `int`, and #6527 already checks that slot. A row below
// pins that the dictionary is still rejected — once, by #6527's own wording.
//
// All rows are on the `runAot` oracle (the in-process JIT harness is
// deprecated), and every rejection row asserts the §5.1 WORDING as well as the
// rejection: `failedAtCompileTimeWithRealLocation` alone is satisfied by a
// crash or an IR-verification failure — which is exactly what four of these
// programs produced before the fix.

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"  // runAot
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

std::string prog(const std::string& mainBody, const std::string& decls = "") {
    return "module M;\n" + decls +
           "class C\n  public static method Main(): int\n" + mainBody +
           "  end Main;\nend C;\n";
}

// The rejection oracle. `r.ran == false` alone is satisfied by a crash, a
// signal, a timeout, an ASan report and an IR-verification failure alike, so
// it discriminates nothing on its own; the WORDING does.
bool rejectedWith(const JitResult& r, const char* msg) {
    return failedAtCompileTimeWithRealLocation(r) &&
           diagnosticMessageContains(r, msg);
}

// stdout of a program that must compile, run and exit 0. `ran == false` reads
// back as a marker no expectation equals, so a rejection FAILS an accept row
// rather than passing it.
std::string out(const std::string& mainBody, const std::string& decls = "") {
    auto r = runAot(prog(mainBody, decls));
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

// The §5.1 wording this fix introduces. Pinned as a whole so a reworded
// diagnostic is a visible change rather than a silent one.
const char* kNotConvertible =
    "has a type that is not implicitly convertible to the target type (§5.1)";

}  // namespace

// ── `Array<T>` — the issue's own family (stdlib §4.4) ─────────────────────

// The issue's own grid, row 1: a `double` index. Pre-fix this reached codegen
// and the LLVM verifier rejected the module ("SExt only operates on integer",
// `sext double 1.500000e+00 to i64`) — an internal compiler error where §5.1
// asks for a diagnostic.
BEZEL_TEST(indexer_index_argument_type_7064,
           array_index_rejects_a_double_index_spec_23_4) {
    auto r = runAot(prog("    var a := new Array<int>(10);\n"
                         "    var v := a[1.5];\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "index argument to `Array[i]`"));
}

// Row 2: a `string` index. §5.1 has no row for it, and the pre-fix compiler
// emitted `sext ptr @.bz.str to i64` — the verifier abort.
BEZEL_TEST(indexer_index_argument_type_7064,
           array_index_rejects_a_string_index_spec_23_4) {
    auto r = runAot(prog("    var a := new Array<int>(10);\n"
                         "    var s: string := \"x\";\n"
                         "    var v := a[s];\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
}

// Row 3: a USER RECORD index. The issue is explicit that the record's NAME is
// irrelevant — "it reproduces identically at a user record named `Foo`" — so
// this row uses a name that is not `Range` and pins the mechanism rather than
// a spelling. Pre-fix: `sext %record.Pair %v to i64`, the verifier abort.
BEZEL_TEST(indexer_index_argument_type_7064,
           array_index_rejects_a_record_index_spec_23_4) {
    auto r = runAot(prog("    var a := new Array<int>(10);\n"
                         "    var p := new Pair(2, 7);\n"
                         "    var v := a[p];\n"
                         "    return 0;\n",
                     "record Pair(Lo: int, Hi: int);\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
}

// Row 4: a CLASS-INSTANCE index. Pre-fix: `sext ptr %v to i64`.
BEZEL_TEST(indexer_index_argument_type_7064,
           array_index_rejects_a_class_instance_index_spec_23_4) {
    auto r = runAot(prog("    var a := new Array<int>(10);\n"
                         "    var o := new Obj();\n"
                         "    var v := a[o];\n"
                         "    return 0;\n",
                     "class Obj\n  public constructor() end constructor;\n"
                     "end Obj;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
}

// Row 5: a `long` index. This is a BEHAVIOUR FLIP — it compiled and ran
// before this fix. §5.1's table row is `int` -> `long`, never the reverse,
// and §5.1's constant-narrowing clause — the only way a wider integer reaches
// a narrower declared type — applies to COMPILE-TIME CONSTANTS only, so a
// run-time `long` has no row.
BEZEL_TEST(indexer_index_argument_type_7064,
           array_index_rejects_a_long_index_spec_5_1) {
    auto r = runAot(prog("    var a := new Array<int>(10);\n"
                         "    var j: long := 5;\n"
                         "    var v := a[j];\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
}

// Row 6: a `bool` index. §5.1: "Between integers and `bool`" is never
// implicit — the spec's own bullet, quoted in the diagnostic. This is the
// second BEHAVIOUR FLIP: it compiled and ran before this fix.
BEZEL_TEST(indexer_index_argument_type_7064,
           array_index_rejects_a_bool_index_spec_5_1) {
    auto r = runAot(prog("    var a := new Array<int>(10);\n"
                         "    var b: bool := true;\n"
                         "    var v := a[b];\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "between integers and `bool` is never implicit"));
}

// Row 7: a `uint` index. §5.1's never-implicit list names "Between signed
// and unsigned types of the same or narrower width" — `uint` and `int` are
// the same width — so this one is rejected by name. (It compiled before this
// fix, and reached codegen as a `zext` — a third BEHAVIOUR FLIP.)
BEZEL_TEST(indexer_index_argument_type_7064,
           array_index_rejects_a_uint_index_spec_5_1) {
    auto r = runAot(prog("    var a := new Array<int>(10);\n"
                         "    var y: uint := 3;\n"
                         "    var v := a[y];\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(rejectedWith(
        r, "between signed and unsigned types of the same or narrower width "
           "is never implicit"));
}

// ── the ACCEPT controls — §5.1's rows that must keep working ──────────────

// The identity row: an `int` index.
BEZEL_TEST(indexer_index_argument_type_7064,
           array_index_accepts_an_int_index_spec_5_1) {
    BEZEL_EXPECT_EQ(out("    var a: Array<int> := [10, 20, 30];\n"
                        "    Console.WriteLine(a[0]);\n"
                        "    return 0;\n"),
                    "10\n");
}

// §5.1's `char` -> `int` row.
BEZEL_TEST(indexer_index_argument_type_7064,
           array_index_accepts_a_char_index_spec_5_1) {
    BEZEL_EXPECT_EQ(out("    var a: Array<int> := [10, 20, 30];\n"
                        "    var c: char := char(0);\n"
                        "    Console.WriteLine(a[c]);\n"
                        "    return 0;\n"),
                    "10\n");
}

// §5.1's `byte` -> `int` row.
BEZEL_TEST(indexer_index_argument_type_7064,
           array_index_accepts_a_byte_index_spec_5_1) {
    BEZEL_EXPECT_EQ(out("    var a: Array<int> := [10, 20, 30];\n"
                        "    var y: byte := 1;\n"
                        "    Console.WriteLine(a[y]);\n"
                        "    return 0;\n"),
                    "20\n");
}

// §5.1's CONSTANT NARROWING: "the source may be a compile-time constant of
// any integer type — not only `int` — and the target any integer type whose
// range contains its value". A `const long` whose value fits an `int` is
// admitted, even though a run-time `long` is not (row 5 above). This is the
// pair that keeps the constant-narrowing clause honest.
BEZEL_TEST(indexer_index_argument_type_7064,
           array_index_accepts_a_constant_long_index_spec_5_1) {
    BEZEL_EXPECT_EQ(out("    var a: Array<int> := [10, 20, 30];\n"
                        "    const N: long := 1;\n"
                        "    Console.WriteLine(a[N]);\n"
                        "    return 0;\n"),
                    "20\n");
}

// §15.6's RANGE slicing form is not an element index: `arr[0..1]` produces a
// fresh owned `Array<T>`. It must keep compiling — and it must not be
// reported as a §5.1 type error.
BEZEL_TEST(indexer_index_argument_type_7064,
           array_range_slice_index_is_not_an_element_index_spec_15_6) {
    BEZEL_EXPECT_EQ(out("    var a: Array<int> := [10, 20, 30];\n"
                        "    using var s := a[0..1];\n"
                        "    Console.WriteLine(s.Count);\n"
                        "    return 0;\n"),
                    "2\n");
}

// ── the other int-indexed families ────────────────────────────────────────

// The span family (stdlib §4.15 `ArraySpan<T>`). Pre-fix `sp[1.5]` compiled
// and ran.
BEZEL_TEST(indexer_index_argument_type_7064,
           span_index_rejects_a_double_index_spec_23_4) {
    auto r = runAot(prog("    var a := new Array<int>(10);\n"
                         "    var sp := a.AsSpan();\n"
                         "    var v := sp[1.5];\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "index argument to `ArraySpan[i]`"));
}

// The list interfaces (stdlib §4.1 `IList<T>`). Pre-fix this reached codegen
// and the verifier rejected the module.
BEZEL_TEST(indexer_index_argument_type_7064,
           list_interface_index_rejects_a_double_index_spec_23_4) {
    auto r = runAot(prog("    var a := new Array<int>(10);\n"
                         "    var il: IList<int> := a;\n"
                         "    var v := il[1.5];\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "index argument to `IList[i]`"));
}

// `OwningArray<T>` (stdlib §4.5) — the owning family, whose indexer is
// declared `[i: int]: T` like `Array<T>`'s.
BEZEL_TEST(indexer_index_argument_type_7064,
           owning_array_index_rejects_a_double_index_spec_23_4) {
    auto r = runAot(prog("    using var a := new OwningArray<Obj>(10);\n"
                         "    var v := a[1.5];\n"
                         "    return 0;\n",
                     "class Obj\n  public constructor() end constructor;\n"
                     "end Obj;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "index argument to `OwningArray[i]`"));
}

// `FrozenArray<T>` (stdlib §4.19). Its receiver arrives as a builtin CLASS
// SYMBOL (the `From(...)` factory result), a different TypeRef shape from
// the other families' — so it is a separate row.
BEZEL_TEST(indexer_index_argument_type_7064,
           frozen_array_index_rejects_a_double_index_spec_23_4) {
    auto r = runAot(prog("    using var a := FrozenArray<int>.From([1, 2, 3]);\n"
                         "    var v := a[1.5];\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "index argument to `FrozenArray[i]`"));
}

// `string` (spec §4.6): `s[i]` is the code-point position. Pre-fix
// `s[1.5]` emitted `sext double 1.500000e+00 to i64` — the verifier abort.
BEZEL_TEST(indexer_index_argument_type_7064,
           string_index_rejects_a_double_index_spec_4_6) {
    auto r = runAot(prog("    var s: string := \"abc\";\n"
                         "    var v := s[1.5];\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
}

// An INLINE ARRAY (spec §15.1). Pre-fix `a[1.5]` emitted the same malformed
// `sext` and the verifier abort.
BEZEL_TEST(indexer_index_argument_type_7064,
           inline_array_index_rejects_a_double_index_spec_15_1) {
    auto r = runAot(prog("    var a: [3]int := [1, 2, 3];\n"
                         "    var v := a[1.5];\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
}

// ── a USER-DECLARED indexer (§23.4) ───────────────────────────────────────

// §23.4's own case: `property [i: int]: int => i * 2`, indexed with a
// `double`. Pre-fix this COMPILED and PRINTED GARBAGE (972021523) — the
// argument reached the accessor with no conversion at all.
BEZEL_TEST(indexer_index_argument_type_7064,
           user_indexer_rejects_a_double_index_spec_23_4) {
    auto r = runAot(prog("    var b := new Box();\n"
                         "    Console.WriteLine(b[1.5]);\n"
                         "    return 0;\n",
                     "public class Box\n"
                     "  public property [i: int]: int => i * 2;\n"
                     "end Box;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "index argument to `Box[i]`"));
}

// The same user indexer with a `string` PARAMETER, indexed with an `int`:
// §23.4 overloads indexers by their parameter TYPE LIST, so an `int` argument
// fits no declared parameter list at all.
BEZEL_TEST(indexer_index_argument_type_7064,
           user_indexer_rejects_an_int_at_a_string_parameter_spec_23_4) {
    auto r = runAot(prog("    var b := new Box();\n"
                         "    var v := b[5];\n"
                         "    return 0;\n",
                     "public class Box\n"
                     "  public property [key: string]: string => \"k\";\n"
                     "end Box;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
}

// The accept control for the user-declared form: a `string` argument at a
// `string` parameter.
BEZEL_TEST(indexer_index_argument_type_7064,
           user_indexer_accepts_a_matching_index_spec_23_4) {
    BEZEL_EXPECT_EQ(out("    var b := new Box();\n"
                        "    Console.WriteLine(b[\"k\"]);\n"
                        "    return 0;\n",
                        "public class Box\n"
                        "  public property [key: string]: string => \"k\";\n"
                        "end Box;\n"),
                    "k\n");
}

// A `long` index at a USER indexer's `int` parameter — the same §5.1 verdict
// as the array row above, on the family whose parameter type comes from the
// USER's own declaration rather than from the standard library.
BEZEL_TEST(indexer_index_argument_type_7064,
           user_indexer_rejects_a_long_index_spec_5_1) {
    auto r = runAot(prog("    var b := new Box();\n"
                         "    var j: long := 5;\n"
                         "    var v := b[j];\n"
                         "    return 0;\n",
                         "public class Box\n"
                         "  public property [i: int]: int => i * 2;\n"
                         "end Box;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "index argument to `Box[i]`"));
}

// A `long` index at `string`'s code-point position (spec §4.6). §4.6's
// position is an integer one, and §5.1 has no `long` -> `int` row.
BEZEL_TEST(indexer_index_argument_type_7064,
           string_index_rejects_a_long_index_spec_5_1) {
    auto r = runAot(prog("    var s: string := \"abc\";\n"
                         "    var j: long := 1;\n"
                         "    var v := s[j];\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "index argument to `string[i]`"));
}

// ── the DICTIONARY family is NOT this fix's business ──────────────────────

// #6527 owns the dictionary's key slot (its index parameter is the key type
// `K`, not `int`). This fix must not report it a SECOND time, and the
// dictionary must still be rejected — by #6527's own wording.
BEZEL_TEST(indexer_index_argument_type_7064,
           dictionary_double_key_is_still_rejected_once_spec_5_1) {
    auto r = runAot(prog("    var d: Dictionary<int, string> := [0: \"AA\"];\n"
                         "    var v := d[3.5];\n"
                         "    return 0;\n"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedWith(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(rejectedWith(r, "index argument to `Dictionary[key]`"));
    // Exactly one report: #6527's own wording, and this fix's wording is
    // NOT among them (the two differ in the subject they name).
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "index argument to "
                                                 "`Dictionary[i]`"));
}

// ── the FAIL-OPEN boundary: an INHERITED generic indexer ────────────────

// A declared index-parameter type read off an INHERITED generic member can
// still carry the DERIVED class's type parameter: the base-chain substitution
// binds `Root`'s `T` to `Array<U>` — with `U` STILL `Mid`'s parameter, one
// level down — so the DECLARATION's parameter type is `Array<U>`, not
// `Array<int>`. §14.2 rule 2 reads the interior use at the INSTANTIATIONS
// the declaration forms, so the §5.1 verdict is undecidable at the
// declaration and the check FAILS OPEN there.
//
// This row is the fail-open guard, and it is not hypothetical: an earlier
// revision of this fix read `Array<U>` as a CONCRETE type and REJECTED this
// program — a false reject that turned the codegen row
// `indexer_vtable_dispatch_6319.ancestor_chain_substitutes_generic_key`
// (#6319) RED. The program must compile, run and exit 1.
BEZEL_TEST(indexer_index_argument_type_7064,
           inherited_generic_indexer_accepts_a_matching_index_spec_14_2) {
    auto r = runAot(
        "module M; "
        "public class Root<T> "
        "  public constructor() end constructor; "
        "  public virtual property [i: T]: string => \"root\"; "
        "end Root; "
        "public class Mid<U>: Root<Array<U>> "
        "  public constructor() end constructor; "
        "end Mid; "
        "public class Program public static method Main(): int "
        "  using var m := new Mid<int>(); "
        "  const a: Array<int> := [1, 2]; "
        "  if m[a] = \"root\" then return 1; end; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    // And nothing was reported at the index position.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kNotConvertible));
}
