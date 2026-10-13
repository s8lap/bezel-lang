// fix #6128 (stdlib §1 "Count vs Length", §3.1, §4.1, §4.14, §5, v0.338):
// the CALL spellings of the member-set rejections #6081 / #6125 left alone.
//
// The stdlib declares exactly one of `Count` / `Length` per type:
//   - `StringBuilder` declares `Length` (§3.1) and no `Count`; it is not an
//     `IEnumerable`, so `sb.Count()` is not the §5 LINQ operator either;
//   - `OwningBuffer<T>` declares `Length` (§4.14); for a VALUE-typed `T` it
//     "does not implement IEnumerable<T>", so `b.Count()` is no LINQ call;
//     for a class `T` it is `IEnumerable<T?>` and `b.Count()` is §5's `Count()`;
//   - `ICollection<T>` / `IList<T>` / `IReadOnlyList<T>` /
//     `IReadOnlyCollection<T>` declare `Count` (§4.1) and no `Length`, and §5
//     has no `Length()` operator.
//
// Rows (`c.M()` and `c?.M()` call forms; the property twins are #6081/#6125):
//   R1  IList<int>.Length()              R2  IList<int>?.Length()
//   R3  IReadOnlyList<int>.Length()      R4  IReadOnlyList<int>?.Length()
//   R5  ICollection<int>?.Length()       R6  IReadOnlyCollection<int>?.Length()
//   R7  StringBuilder.Count()            R8  StringBuilder?.Count()
//   R9  OwningBuffer<byte>.Count()       R10 OwningBuffer<byte>?.Count()
//   C1  Array<int>.Count() runs (§5 LINQ over IEnumerable; §4.4)
//   C2  OwningBuffer<Foo>.Count() / ?.Count() accepted (class T: IEnumerable)
//   C3  user class with its own Count()/Length() methods runs
//   C4  user class with function-typed fields named Length/Count is accepted
//
// The rejections are an implementation convention pending the owner's ruling
// on the #6071 spec question (which member a type lacks).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

#define BEZEL_CALL_REJECT(NAME, MODULE, DECL, EXPR, NEEDLE)               \
    BEZEL_TEST(sema_member_set_call_spellings_6128, NAME) {              \
        auto r = runAot("module " MODULE "; "                            \
                        "class Program public static method Main(): int " \
                        "  " DECL " "                                    \
                        "  var x := " EXPR "; "                          \
                        "  return 0; "                                   \
                        "end Main; end Program;");                       \
        BEZEL_EXPECT_FALSE(r.ran);                                       \
        BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));       \
        BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, NEEDLE));         \
    }

BEZEL_CALL_REJECT(ilist_length_call_rejected_stdlib_4_1, "M6128a",
                  "var c: IList<int> := new Array<int>();", "c.Length()",
                  "no member `Length` on `IList<T>`: use `Count`")
BEZEL_CALL_REJECT(ilist_nullcond_length_call_rejected_stdlib_4_1, "M6128b",
                  "var c: IList<int>? := null;", "c?.Length()",
                  "no member `Length` on `IList<T>`: use `Count`")
BEZEL_CALL_REJECT(ireadonlylist_length_call_rejected_stdlib_4_1, "M6128c",
                  "var c: IReadOnlyList<int> := new Array<int>();",
                  "c.Length()",
                  "no member `Length` on `IReadOnlyList<T>`: use `Count`")
BEZEL_CALL_REJECT(ireadonlylist_nullcond_length_call_rejected_stdlib_4_1,
                  "M6128d", "var c: IReadOnlyList<int>? := null;",
                  "c?.Length()",
                  "no member `Length` on `IReadOnlyList<T>`: use `Count`")
BEZEL_CALL_REJECT(icollection_nullcond_length_call_rejected_stdlib_4_1,
                  "M6128e", "var c: ICollection<int>? := null;", "c?.Length()",
                  "no member `Length` on `ICollection<T>`: use `Count`")
BEZEL_CALL_REJECT(ireadonlycollection_nullcond_length_call_rejected_stdlib_4_1,
                  "M6128f", "var c: IReadOnlyCollection<int>? := null;",
                  "c?.Length()",
                  "no member `Length` on `IReadOnlyCollection<T>`: use `Count`")
BEZEL_CALL_REJECT(stringbuilder_count_call_rejected_stdlib_3_1, "M6128g",
                  "var sb := new StringBuilder();", "sb.Count()",
                  "no member `Count` on `StringBuilder`: use `Length`")
BEZEL_CALL_REJECT(stringbuilder_nullcond_count_call_rejected_stdlib_3_1,
                  "M6128h", "var sb: StringBuilder? := null;", "sb?.Count()",
                  "no member `Count` on `StringBuilder`: use `Length`")
BEZEL_CALL_REJECT(owningbuffer_value_count_call_rejected_stdlib_4_14,
                  "M6128i", "using var b := new OwningBuffer<byte>(4);",
                  "b.Count()",
                  "no member `Count` on `OwningBuffer<T>`: use `Length`")
BEZEL_CALL_REJECT(owningbuffer_value_nullcond_count_call_rejected_stdlib_4_14,
                  "M6128j", "var b: OwningBuffer<byte>? := null;",
                  "b?.Count()",
                  "no member `Count` on `OwningBuffer<T>`: use `Length`")

// ── C1 — LINQ `Count()` on an IEnumerable receiver stays accepted (§5, §4.4).
BEZEL_TEST(sema_member_set_call_spellings_6128,
           array_count_call_runs_stdlib_5) {
    auto r = runAot(
        "module M6128k; "
        "class Program public static method Main(): int "
        "  var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  Console.WriteLine(a.Count()); "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

// ── C2 — class-typed OwningBuffer is IEnumerable<T?> (§4.14): `Count()` is
// the §5 operator and stays accepted (both spellings); the declared `Length`
// still runs.
BEZEL_TEST(sema_member_set_call_spellings_6128,
           owningbuffer_class_count_call_accepted_stdlib_4_14) {
    auto r = runAot(
        "module M6128l; "
        "class Foo public method Z(): int return 0; end Z; end Foo; "
        "class Program public static method Main(): int "
        "  using var b := new OwningBuffer<Foo>(3); "
        "  var n := b.Count(); "
        "  var m: OwningBuffer<Foo>? := null; "
        "  var k := m?.Count(); "
        "  Console.WriteLine(b.Length); "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

// ── C3 — a user type's own `Count()` / `Length()` methods are untouched.
BEZEL_TEST(sema_member_set_call_spellings_6128,
           user_class_own_count_length_methods_run) {
    auto r = runAot(
        "module M6128m; "
        "class UC public method Count(): int return 11; end Count; "
        "  public method Length(): int return 22; end Length; end UC; "
        "class Program public static method Main(): int "
        "  var u := new UC(); "
        "  Console.WriteLine(u.Count()); "
        "  Console.WriteLine(u.Length()); "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("11\n22\n"));
}

// ── C4 — a call through a function-typed FIELD named Length/Count is not a
// member-set read of a built-in head; the user class is unaffected.
BEZEL_TEST(sema_member_set_call_spellings_6128,
           user_class_function_field_length_call_runs) {
    auto r = runAot(
        "module M6128n; "
        "class FC public Length: () -> int; "
        "  public constructor() Length := lambda() => 3; end constructor; "
        "end FC; "
        "class Program public static method Main(): int "
        "  var f := new FC(); "
        "  Console.WriteLine(f.Length()); "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}
