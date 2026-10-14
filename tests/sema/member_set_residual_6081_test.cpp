// fix #6081 (stdlib §3.1, §4.1, §4.14, v0.338; spec §7.5): residual `.` and
// `?.` member-set false accepts left after #6071.
//
// The stdlib names exactly one of `Count` / `Length` on each type below:
//   - `StringBuilder` declares `Length` (the extent) and no `Count` (§3.1);
//   - `OwningBuffer<T>` declares `Length` (stored extent, immutable once
//     allocated) and no `Count` (§4.14);
//   - `ICollection<T>` declares `Count`, inherited from IReadOnlyCollection<T>
//     (§4.1), and no `Length`.
// The bare reads (`sb.Count`, `sb?.Count`, `b.Count`, `b?.Count`,
// `c.Length`, `c?.Length`) were accepted; each is now a "no member" reject
// that names the declared member. The call spellings (`b.Count()`, the §5 LINQ
// operator) are untouched, as is the #6071 `string` arm.
//
// Rows:
//   R1  `ICollection<int>?.Length`  — rejected, names `Count` (§4.1).
//   R2  `ICollection<int>.Length`   — rejected, names `Count` (§4.1).
//   R3  `StringBuilder?.Count`      — rejected, names `Length` (§3.1).
//   R4  `StringBuilder.Count`       — rejected, names `Length` (§3.1).
//   R5  `OwningBuffer<int>.Count`   — rejected, names `Length` (§4.14).
//   R6  `OwningBuffer<int>?.Count`  — rejected, names `Length` (§4.14).
//   C1  `StringBuilder.Length` runs; `StringBuilder?.Length` is accepted (§3.1).
//   C2  `OwningBuffer<int>.Length` runs (§4.14 control).
//   C3  `Array<int>.Count` runs; `Array<int>?.Count` is accepted (§4.4).
//
// The rejections are an implementation convention pending the owner's ruling
// on the #6071 spec question (which member a type lacks); no spec sentence
// names it.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ── R1 — `?.Length` on an interface head that declares only Count (§4.1).
BEZEL_TEST(sema_member_set_residual_6081,
           icollection_nullcond_length_rejected_stdlib_4_1) {
    auto r = runAot(
        "module M6081a; "
        "class Program public static method Main(): int "
        "  var c: ICollection<int>? := null; "
        "  var x := c?.Length; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Length` on `ICollection<T>`: use `Count`"));
}

// ── R2 — `.Length` on the same interface head: the `.` form rejects it too.
BEZEL_TEST(sema_member_set_residual_6081,
           icollection_dot_length_rejected_stdlib_4_1) {
    auto r = runAot(
        "module M6081b; "
        "class Program public static method Main(): int "
        "  var c: ICollection<int> := new Array<int>(); "
        "  var x := c.Length; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Length` on `ICollection<T>`: use `Count`"));
}

// ── R3 — `?.Count` on StringBuilder: §3.1 declares Length, not Count.
BEZEL_TEST(sema_member_set_residual_6081,
           stringbuilder_nullcond_count_rejected_stdlib_3_1) {
    auto r = runAot(
        "module M6081c; "
        "class Program public static method Main(): int "
        "  var sb: StringBuilder? := null; "
        "  var x := sb?.Count; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Count` on `StringBuilder`: use `Length`"));
}

// ── R4 — `.Count` on StringBuilder: the `.` form accepted it (the codegen
// read returned the part-length sum, a Length value under a Count name).
BEZEL_TEST(sema_member_set_residual_6081,
           stringbuilder_dot_count_rejected_stdlib_3_1) {
    auto r = runAot(
        "module M6081d; "
        "class Program public static method Main(): int "
        "  var sb := new StringBuilder(); "
        "  sb.Append(\"abc\"); "
        "  var x := sb.Count; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Count` on `StringBuilder`: use `Length`"));
}

// ── R5 — `.Count` on OwningBuffer<T>: §4.14 declares Length, no Count.
BEZEL_TEST(sema_member_set_residual_6081,
           owningbuffer_dot_count_rejected_stdlib_4_14) {
    auto r = runAot(
        "module M6081e; "
        "class Program public static method Main(): int "
        "  using var b := new OwningBuffer<int>(3); "
        "  var x := b.Count; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Count` on `OwningBuffer<T>`: use `Length`"));
}

// ── R6 — `?.Count` on OwningBuffer<T>: the `?.` twin of R5.
BEZEL_TEST(sema_member_set_residual_6081,
           owningbuffer_nullcond_count_rejected_stdlib_4_14) {
    auto r = runAot(
        "module M6081f; "
        "class Program public static method Main(): int "
        "  var b: OwningBuffer<int>? := null; "
        "  var x := b?.Count; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Count` on `OwningBuffer<T>`: use `Length`"));
}

// ── C1 — the declared member: StringBuilder `.Length` runs (§3.1). The `?.`
// form is accepted (`r.ran`: the program compiles and runs); its VALUE lowering on a
// StringBuilder head is pinned by the #6024 value rows (null_cond_count_length_typing_6024_test.cpp),
// so this test does not assert its stdout.
BEZEL_TEST(sema_member_set_residual_6081,
           stringbuilder_length_dot_runs_stdlib_3_1) {
    auto r = runAot(
        "module M6081g; "
        "class Program public static method Main(): int "
        "  var sb := new StringBuilder(); "
        "  sb.Append(\"abc\"); "
        "  Console.WriteLine(sb.Length); "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

BEZEL_TEST(sema_member_set_residual_6081,
           stringbuilder_length_nullcond_accepted_stdlib_3_1) {
    auto r = runAot(
        "module M6081j; "
        "class Program public static method Main(): int "
        "  var sn: StringBuilder? := null; "
        "  var x := sn?.Length; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
}

// ── C2 — the declared member: OwningBuffer `.Length` runs (§4.14).
BEZEL_TEST(sema_member_set_residual_6081,
           owningbuffer_length_dot_runs_stdlib_4_14) {
    auto r = runAot(
        "module M6081h; "
        "class Program public static method Main(): int "
        "  using var b := new OwningBuffer<int>(4); "
        "  Console.WriteLine(b.Length); "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\n"));
}

// ── C3 — the declared member on the resizable Array: `.Count` runs (§4.4).
// The `?.Count` form is accepted (`r.ran`); its value lowering on a collection head is pinned by the #6024 value rows (null_cond_count_length_typing_6024_test.cpp).
BEZEL_TEST(sema_member_set_residual_6081,
           array_count_dot_runs_stdlib_4_4) {
    auto r = runAot(
        "module M6081i; "
        "class Program public static method Main(): int "
        "  var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); "
        "  Console.WriteLine(a.Count); "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

BEZEL_TEST(sema_member_set_residual_6081,
           array_count_nullcond_accepted_stdlib_4_4) {
    auto r = runAot(
        "module M6081k; "
        "class Program public static method Main(): int "
        "  var an: Array<int>? := null; "
        "  var x := an?.Count; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
}
