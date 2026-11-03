// fix #8020 (stdlib §3.1, §7.3, v0.350) — the §7.3 NULLABILITY of three
// `StringBuilder` parameter positions.
//
// --- THE NORMATIVE SENTENCES (CHECKED in docs/, v0.350) ----------------------
//
// stdlib §3.1 `StringBuilder` declares, verbatim:
//
//   public method Insert(index: int, value: string): StringBuilder;
//   public method Remove(startIndex: int, length: int): StringBuilder;
//
// — NEITHER parameter carries a `?`, so every one of those positions is
// NON-nullable.
//
// spec §7.3 "Assigning `null` to a non-nullable target": "Assigning `null` to
// a non-nullable target is a compile error."  spec §7.1: a nullable value
// does not implicitly convert to its non-nullable form.
//
// --- THE DEFECT (MEASURED on this lane's base) ------------------------------
//
// The ONE built-in parameter table (`src/common/BuiltinRegistry.cpp`'s
// `kBuiltinParams`) carries, per DECLARED FORM, the parameter NAME (§22.5),
// the parameter TYPE (§5.1) and the parameter NULLABILITY (§7.3).  The
// `nonNullable` mask of the `Insert` row was `0x02` (the VALUE position
// only) and the `Remove` row's was `0x00` — so the table did not STATE the
// nullability the document states, and the §7.3 gate (which reads the mask)
// had nothing to read at those three positions.
//
// The table now states the document's own nullability (`Insert`'s mask is
// `0x03` and `Remove`'s is `0x03`), and the rows below pin the BEHAVIOUR
// that follows: a NULLABLE argument at one of those positions is refused with
// §7's own message.
//
// The ratchet (`scripts/check_builtin_declared_params.py`) is the other half
// of the fix, and it is proved by the checker's own self-test: a mutation
// that CLEARS the §7.3 bit at the `Remove` position 0 makes the checker exit
// non-zero and name the member and the position.
//
// Bezel syntax notes: a nullable `int` comes from a method whose return type
// is `int?`; the value is not narrowed, so it is still nullable at the call.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// A program with a nullable `int` in scope.
std::string prog(const std::string& body) {
    return "module M; import System; import System.Text; "
           "class C "
           "  public static method Id(x: int): int? return x; end Id; end C; "
           "class D "
           "  public static method Main(): int "
           "    var sb := new StringBuilder(); "
           "    var i: int? := C.Id(5); " +
           body +
           "    return 0; "
           "  end Main; "
           "end D;";
}

// A genuine compile-time rejection carrying `needle` in its message.
bool rejectsWith(const std::string& src, const char* needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    if (!failedAtCompileTimeWithRealLocation(r)) return false;
    return diagnosticMessageContains(r, needle);
}

// §7's nullability message — the house wording the §7.3 gate produces.
const char* kNullableMessage =
    "is nullable but the target type is non-nullable (\xC2\xA7""7)";

}  // namespace

// --- THE THREE POSITIONS: a nullable argument is REFUSED -------------------

// stdlib §3.1 declares `Insert(index: int, value: string)` — `index` is not
// nullable, so a nullable `int` reaches it by no conversion at all (§7.1).
BEZEL_TEST(builtin_nullability_declared_8020,
           insert_nullable_index_is_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("    sb.Insert(i, \"x\");"),
        "the argument to `StringBuilder.Insert` is nullable but the target "
        "type is non-nullable (\xC2\xA7""7)"));
}

// stdlib §3.1 declares `Remove(startIndex: int, length: int)` — `startIndex`
// is not nullable.
BEZEL_TEST(builtin_nullability_declared_8020,
           remove_nullable_startindex_is_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("    sb.Remove(i, 1);"),
        "the argument to `StringBuilder.Remove` is nullable but the target "
        "type is non-nullable (\xC2\xA7""7)"));
}

// stdlib §3.1 declares `Remove(startIndex: int, length: int)` — `length` is
// not nullable.
BEZEL_TEST(builtin_nullability_declared_8020,
           remove_nullable_length_is_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("    sb.Remove(0, i);"),
        "the argument to `StringBuilder.Remove` is nullable but the target "
        "type is non-nullable (\xC2\xA7""7)"));
}

// --- THE CONTROLS: the legal spellings still work --------------------------

// The legal `int` arguments at the same three positions still compile and
// run — the rows above refuse the NULLABILITY, not the call.
BEZEL_TEST(builtin_nullability_declared_8020, int_arguments_are_admitted) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; import System; import System.Text; "
            "class C "
            "  public static method Main(): int "
            "    var sb := new StringBuilder(); "
            "    sb.Append(\"abc\"); "
            "    sb.Insert(0, \"x\"); "
            "    Console.WriteLine(sb.ToString()); "
            "    sb.Remove(0, 1); "
            "    Console.WriteLine(sb.ToString()); "
            "    return 0; "
            "  end Main; "
            "end C;"),
        "xabc\nabc");
}

// A §7.4-NARROWED nullable is of its non-nullable type (§22.6: "a read of a
// binding §7.4 has narrowed non-null is of its non-nullable type"), so it
// still reaches the position.
BEZEL_TEST(builtin_nullability_declared_8020,
           narrowed_nullable_is_admitted) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; import System; import System.Text; "
            "class C "
            "  public static method Id(x: int): int? return x; end Id; end C; "
            "class D "
            "  public static method Main(): int "
            "    var sb := new StringBuilder(); "
            "    sb.Append(\"abcd\"); "
            "    var i: int? := C.Id(2); "
            "    if i is not null then "
            "      sb.Remove(0, i); "
            "    end if; "
            "    Console.WriteLine(sb.ToString()); "
            "    return 0; "
            "  end Main; "
            "end D;"),
        "cd");
}
