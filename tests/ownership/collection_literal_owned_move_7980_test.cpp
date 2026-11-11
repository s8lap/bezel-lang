// Spec-derived regression tests for issue #7980: a collection-literal local
// bound with an ANNOTATED type (`var a: Array<int> := [1];`) was classified a
// BORROW, so `Take(move a)` into an `owned` parameter was rejected with
// §28.8's "a borrow cannot be passed where an `owned` value is required".
//
// The spec sentences that decide the outcome (v0.357):
//
//   §28.3.4, the producers table: "Collection literal (`[1, 2, 3]`)
//   targeting a reference type (`Array<T>`, `OwningArray<T>`, `Set<T>`,
//   `Dictionary<K, V>`, etc.) | **Owned**".
//
//   §28.3.3: "`var x := expr;` takes the ownership state of `expr`".
//
//   §15.4: "A collection literal has no standalone type. It is well-typed
//   only in a context that provides the target type" -- so the literal's
//   produced type is the local's DECLARED type, and sema records no
//   `exprType` for the literal node itself (why the local fell to Borrowed).
//
// => `a` owns the literal, `move a` is an owned value (§28.8.1), and the
// `owned` parameter may take it, exactly as it takes the `new Array<int>()`
// twin's.
//
// ORACLES. The accept rows use `runAotChecked` (compiled, linked, run; the
// ASan leg runs under BEZEL_AOT_FORCE_SANITIZE=address). On the pre-fix
// compiler the issue's program is rejected at compile time, so its row is
// RED there. The loop rows repeat the transfer 200 times so a per-iteration
// leak of the moved literal is visible to LeakSanitizer. The reject rows
// assert the specific diagnostic.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The §28.8 owned sink: it adopts and disposes what it is given.
constexpr const char* kOwnedArraySink =
    "  public static method Take(x: owned Array<int>): int "
    "    using var hold := move x; "
    "    return hold.Count; "
    "  end Take; ";

std::string program(const char* sink, const char* body) {
    return std::string("module M; import System; class C ") + sink +
           "  public static method Main(): int " + body +
           "  end Main; "
           "end C;";
}

}  // namespace

// THE ISSUE'S OWN REPRO (§28.3.4 producers row, §28.8.1, v0.357). RED on the
// pre-fix compiler: "a borrow cannot be passed where an `owned` value is
// required".
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           annotated_literal_moved_into_owned_param_spec_28_3_4) {
    auto r = runAotChecked(program(
        kOwnedArraySink,
        "    var a: Array<int> := [1]; "
        "    var n := Take(move a); "
        "    Console.WriteLine(n); "
        "    return 0; "));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// The same transfer 200 times: the moved literal is disposed once by the
// sink, never leaked and never freed again by the caller's scope end
// (§28.3.4, §28.8, §15.4 "An `owned` parameter takes the aggregate by
// transfer", v0.357). RED on the pre-fix compiler (rejected).
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           annotated_literal_moved_in_loop_disposed_once_spec_28_8) {
    auto r = runAotChecked(program(
        kOwnedArraySink,
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var a: Array<int> := [1, 2, 3]; "
        "      acc := acc + Take(move a); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc); "
        "    return 0; "));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("600\n"));
}

// A second head named by the producers row: `Dictionary<K, V>` (§28.3.4,
// v0.357). RED on the pre-fix compiler (rejected).
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           annotated_dictionary_literal_moved_into_owned_param_spec_28_3_4) {
    auto r = runAotChecked(program(
        "  public static method Take(x: owned Dictionary<string, int>): int "
        "    using var hold := move x; "
        "    return hold.Count; "
        "  end Take; ",
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var d: Dictionary<string, int> := [\"a\": 1, \"b\": 2]; "
        "      acc := acc + Take(move d); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc); "
        "    return 0; "));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("400\n"));
}

// The admitting twin: the `new` spelling of the same producers row was
// admitted all along. GREEN on both sides -- it shows the annotated row is
// refused before the fix for the classification alone.
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           new_form_twin_moved_into_owned_param_spec_28_3_4) {
    auto r = runAotChecked(program(
        kOwnedArraySink,
        "    var a := new Array<int>(); "
        "    a.Add(1); "
        "    var n := Take(move a); "
        "    Console.WriteLine(n); "
        "    return 0; "));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// The owned sink is not widened: the UNMARKED read of the now-owned local
// into an `owned` parameter is §22.3's marker error, the one the `new` twin
// draws (§22.3, v0.357). Before the fix this program drew §28.8's borrow
// diagnostic instead, so the row is RED there on its needle.
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           annotated_literal_unmarked_into_owned_param_rejected_spec_22_3) {
    auto r = runAot(program(
        kOwnedArraySink,
        "    var a: Array<int> := [1]; "
        "    var n := Take(a); "
        "    Console.WriteLine(n); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "write `M(move a)`"));
}

// A read after the transfer is a use of a moved-from local (§28.3.3,
// v0.357).
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           annotated_literal_read_after_move_rejected_spec_28_3_3) {
    auto r = runAot(program(
        kOwnedArraySink,
        "    var a: Array<int> := [1]; "
        "    var n := Take(move a); "
        "    Console.WriteLine(a.Count); "
        "    return n; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "owned value in `a` was already moved out at this point"));
}

// A transfer on SOME paths only takes the verdict its `new`-form twin takes
// (`var a := new Array<int>(); if c then Take(move a); end if;` is rejected
// with the same diagnostic): the literal local's exemption from the
// scope-exit obligation does not cover a maybe-consumed state (§28.3.3,
// v0.357; recorded interpretation, #7980).
BEZEL_TEST(ownership_collection_literal_owned_move_7980,
           annotated_literal_conditional_move_matches_new_twin_spec_28_3_3) {
    auto r = runAot(program(
        "  public static method Take(x: owned Dictionary<string, int>): int "
        "    using var hold := move x; "
        "    return hold.Count; "
        "  end Take; "
        "  public static method Run(c: bool): int "
        "    var d: Dictionary<string, int> := [\"a\": 1, \"b\": 2]; "
        "    var n := 0; "
        "    if c then n := Take(move d); end if; "
        "    return n; "
        "  end Run; ",
        "    Console.WriteLine(C.Run(true)); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "owned value in `d` would be dropped here without disposal"));
}
