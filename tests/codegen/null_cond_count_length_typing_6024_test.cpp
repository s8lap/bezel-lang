// Sema — issue #6024 (§7.5, v0.338). `c?.Count` / `c?.Length` on a built-in
// collection/string head must carry the nullable result type `int?`.
//
// Spec: §7.5 "Null-conditional access (`?.`)", v0.338 — "`x?.M` returns null
// if `x` is null; otherwise it performs the access. The result type is always
// nullable" (worked example `var length: int? := user?.Name?.Length;`).
// Pre-fix the `?.` node got NO sema type for collection `.Count`, so
// `var n: int := a?.Count` was accepted (with a #2434 degrade warning) and
// `var x := a?.Count; var s: string := x;` was accepted silently.
//
// The `Array`/`Stack` `?.Length` rows keep the `.` form's Count-vs-Length
// rejection (§4.4 / §4.11, #4811 / #5416).
//
// Scope: the VALUE lowering of a collection `?.Count` is a separate codegen
// gap (the degrade warning persists on an accepted `int?` initializer); only
// the typing half is pinned here, plus the already-working `string?.Length`.

#include "support/aot_result_asserts.hpp"
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// §7.5: `?.Count` is `int?`, so it cannot initialise a non-nullable `int`.
BEZEL_TEST(null_cond_count_length_typing_6024,
           count_into_non_nullable_int_rejected_spec_7_5) {
    auto r = runAot(
        "module M6024a; "
        "class Program public static method Main(): int "
        "  var a: Array<int>? := null; "
        "  var n: int := a?.Count; "
        "  return n; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "nullable but the target type is non-nullable"));
}

// §7.5: an inferred `x := a?.Count` is `int?`, not assignable to `string`.
BEZEL_TEST(null_cond_count_length_typing_6024,
           inferred_count_into_string_rejected_spec_7_5) {
    auto r = runAot(
        "module M6024b; "
        "class Program public static method Main(): int "
        "  var a: Array<int>? := null; "
        "  var x := a?.Count; "
        "  var s: string := x; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "nullable but the target type is non-nullable"));
}

// §7.5 + §4.11: `Stack<T>?`'s `?.Count` is `int?` too (second head family).
BEZEL_TEST(null_cond_count_length_typing_6024,
           stack_count_into_non_nullable_int_rejected_spec_7_5) {
    auto r = runAot(
        "module M6024c; "
        "class Program public static method Main(): int "
        "  var st: Stack<int>? := null; "
        "  var n: int := st?.Count; "
        "  return n; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "nullable but the target type is non-nullable"));
}

// §4.4 / #4811: `Array<T>` declares `Count`, not `Length` — same as `.Length`.
BEZEL_TEST(null_cond_count_length_typing_6024,
           array_nullcond_length_rejected_spec_4_4) {
    auto r = runAot(
        "module M6024d; "
        "class Program public static method Main(): int "
        "  var a: Array<int>? := null; "
        "  var x := a?.Length; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Length` on `Array<T>`"));
}

// §4.11 / #5416: `Stack<T>` declares `Count`, not `Length`.
BEZEL_TEST(null_cond_count_length_typing_6024,
           stack_nullcond_length_rejected_spec_4_11) {
    auto r = runAot(
        "module M6024e; "
        "class Program public static method Main(): int "
        "  var st: Stack<int>? := null; "
        "  var x := st?.Length; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Length` on `Stack<T>`"));
}

// §7.5 / stdlib §9.6 pin (already typed pre-fix): `string?.Length` runs; the
// null receiver takes the null branch, the non-null one yields the value.
BEZEL_TEST(null_cond_count_length_typing_6024,
           string_nullcond_length_runs_spec_7_5) {
    auto r = runAot(
        "module M6024f; "
        "class Program public static method Main(): int "
        "  var s: string? := null; "
        "  var t: string? := \"abc\"; "
        "  Console.WriteLine(s?.Length ?? -1); "
        "  Console.WriteLine(t?.Length ?? -1); "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "-1\n3\n");
}

// ─── #6071: the `?.` forms mirror the `.` form's member-surface rejects ───
// stdlib §4.7-§4.10 / §4.17 / §4.18: Set, Dictionary, LinkedList and the
// sorted collections declare `Count`, never `Length`; stdlib §9.6: `string`
// declares `Length`, not `Count`. The `.` form rejects `Set.Length` /
// `Dictionary.Length`; the `?.` form must not be the escape hatch.

static const char* kLengthHeads[][2] = {
    {"Set<int>", "M6071a"},
    {"Dictionary<string, int>", "M6071b"},
    {"LinkedList<int>", "M6071c"},
    {"SortedSet<int>", "M6071d"},
    {"SortedDictionary<int, int>", "M6071e"},
};

BEZEL_TEST(null_cond_count_length_typing_6024,
           nullcond_length_on_non_array_collections_rejected_spec_4) {
    for (auto& h : kLengthHeads) {
        auto r = runAot(
            std::string("module ") + h[1] + "; "
            "class Program public static method Main(): int "
            "  var c: " + h[0] + "? := new " + h[0] + "(); "
            "  var x := c?.Length; "
            "  return 0; "
            "end Main; end Program;");
        BEZEL_EXPECT_FALSE(r.ran);
        BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
        BEZEL_EXPECT_TRUE(diagnosticMessageContains(
            r, "no member `Length` on"));
    }
}

BEZEL_TEST(null_cond_count_length_typing_6024,
           string_nullcond_count_rejected_spec_9_6) {
    auto r = runAot(
        "module M6071f; "
        "class Program public static method Main(): int "
        "  var s: string? := \"abc\"; "
        "  var x := s?.Count; "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no member `Count` on `string`"));
}

// Accept controls: the rejects above must not touch `Set?.Count`,
// `Dictionary?.Count`, the LINQ call spelling `s?.Count()`, or a user class's
// own `Count` / `Length` members (value runs; collection `Count` is only
// compiled here, its value lowering being the separate codegen gap above).
BEZEL_TEST(null_cond_count_length_typing_6024,
           nullcond_count_accept_controls_spec_7_5) {
    auto r = runAot(
        "module M6071g; "
        "class Node public constructor() end constructor; "
        "  public property Count: int := 3; "
        "  public property Length: int := 4; "
        "end Node; "
        "class Program public static method Main(): int "
        "  var st: Set<int>? := new Set<int>(); "
        "  var d: Dictionary<string, int>? := new Dictionary<string, int>(); "
        "  var s: string? := \"abc\"; "
        "  var a: Array<int>? := new Array<int>(); "
        "  var n: Node? := new Node(); "
        "  var x1 := st?.Count; var x2 := d?.Count; "
        "  var x3 := s?.Count(); var x4 := a?.Count(); "
        "  Console.WriteLine(n?.Count ?? -1); "
        "  Console.WriteLine(n?.Length ?? -1); "
        "  Console.WriteLine(s?.Length ?? -1); "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n4\n3\n");
}
