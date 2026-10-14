// fix #6284 — spec §30.14 (v0.338): a lambda's own locals are not captures.
//
// isCaptureFreeEntry's capture walk (exprCaptures / stmtCaptures) treated every
// local or parameter it met as captured unless it was one of the lambda's own
// parameters. A local declared INSIDE the lambda body was therefore a capture,
// and `Domain.Spawn(lambda() => do var k := 4; Console.WriteLine(k); end)` was
// rejected although nothing crosses the boundary. The walk now also admits a
// symbol whose declaring node lies inside the entry lambda's source range, so
// the rule covers every caller of isCaptureFreeEntry: Domain.Spawn (§30.14),
// SharedEventSource Subscribe (§6.5 / #4920), `static event +=` (#4920) and the
// extern callback check (§32.8 / #4671).
//
// Each caller gets an own-local ACCEPT row (plain and interpolated spelling)
// and an outer-capture REJECT row (interpolated spelling, the #6278 shape). The
// Spawn rows add a for-loop variable, a catch variable and a shadowing case.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// A capture-free rejection, by the caller's own diagnostic text.
bool rejectedBy(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

const char* kSpawnNeedle = "Domain.Spawn entry must be capture-free";
const char* kSubscribeNeedle =
    "handler subscribed to an ISharedEvent<T> or SharedEventSource<T> must be "
    "capture-free";
const char* kStaticEventNeedle =
    "handler subscribed to a `static event` must be capture-free";
const char* kExternNeedle =
    "callback argument at a plain (un-`env`-paired) extern function-typed "
    "parameter must be capture-free";

}  // namespace

// ---- Domain.Spawn (§30.14) -------------------------------------------------

BEZEL_TEST(lambda_own_local_not_a_capture_6284,
           spawn_own_local_plain_accepted_spec_30_14) {
    auto r = runAot(
        "module T; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var u := Domain.Spawn(lambda() => do var k := 4; "
        "Console.WriteLine(k); end); "
        "    u.Wait(); "
        "    return 0; "
        "  end; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("4") != std::string::npos);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(lambda_own_local_not_a_capture_6284,
           spawn_own_local_interpolated_accepted_spec_30_14) {
    auto r = runAot(
        "module T; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var u := Domain.Spawn(lambda() => do var k := 4; "
        "Console.WriteLine($\"k={k}\"); end); "
        "    u.Wait(); "
        "    return 0; "
        "  end; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("k=4") != std::string::npos);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSpawnNeedle));
}

// A for-loop variable declared in the body is the lambda's own name.
BEZEL_TEST(lambda_own_local_not_a_capture_6284,
           spawn_own_for_loop_variable_accepted_spec_30_14) {
    auto r = runAot(
        "module T; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var u := Domain.Spawn(lambda() => do "
        "for var i in 1..2 do Console.WriteLine($\"i={i}\"); end; end); "
        "    u.Wait(); "
        "    return 0; "
        "  end; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("i=1") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("i=2") != std::string::npos);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSpawnNeedle));
}

// A catch-arm binding declared in the body is the lambda's own name.
BEZEL_TEST(lambda_own_local_not_a_capture_6284,
           spawn_own_catch_variable_accepted_spec_30_14) {
    auto r = runAot(
        "module T; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    using var u := Domain.Spawn(lambda() => do try "
        "Console.WriteLine(1); catch var ex: Exception => "
        "do var m := ex.Message; Console.WriteLine($\"m={m}\"); end; end; end); "
        "    u.Wait(); "
        "    return 0; "
        "  end; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSpawnNeedle));
}

// An interpolated outer capture stays a capture even when the body also
// declares its own local (the #6278 shape, now with an own local present).
BEZEL_TEST(lambda_own_local_not_a_capture_6284,
           spawn_outer_capture_in_interpolation_still_rejected_spec_30_14) {
    auto r = runAot(
        "module T; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var n := 5; "
        "    using var u := Domain.Spawn(lambda() => do var k := 4; "
        "Console.WriteLine($\"k={k}{n}\"); end); "
        "    u.Wait(); "
        "    return 0; "
        "  end; "
        "end;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

// Shadowing: the first use is the outer `n` (declared before the lambda), so
// the lambda captures it even though the body later declares its own `n`.
BEZEL_TEST(lambda_own_local_not_a_capture_6284,
           spawn_use_before_own_shadow_still_capture_spec_30_14) {
    auto r = runAot(
        "module T; "
        "public static class Program "
        "  public static method Main(args: Array<string>): int "
        "    var n := 1; "
        "    using var u := Domain.Spawn(lambda() => do "
        "Console.WriteLine(n); var n := 2; Console.WriteLine(n); end); "
        "    u.Wait(); "
        "    return 0; "
        "  end; "
        "end;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

// ---- SharedEventSource.Subscribe (§6.5 / #4920) -----------------------------

BEZEL_TEST(lambda_own_local_not_a_capture_6284,
           subscribe_own_local_plain_accepted_spec_6_5) {
    auto r = runAot(
        "module M; import System.Threading; "
        "class Buss public static owned readonly Feed: SharedEventSource<int> := new(); end Buss; "
        "class Cs public static method Main(): int "
        "  var sub := Buss.Feed.Subscribe(lambda(x: int) => do var k := 4; "
        "Console.WriteLine(k); end); "
        "  Buss.Feed.Raise(9); sub.Dispose(); return 0; "
        "end Main; end Cs;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("4") != std::string::npos);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSubscribeNeedle));
}

BEZEL_TEST(lambda_own_local_not_a_capture_6284,
           subscribe_own_local_interpolated_accepted_spec_6_5) {
    auto r = runAot(
        "module M; import System.Threading; "
        "class Buss public static owned readonly Feed: SharedEventSource<int> := new(); end Buss; "
        "class Cs public static method Main(): int "
        "  var sub := Buss.Feed.Subscribe(lambda(x: int) => do var k := 4; "
        "Console.WriteLine($\"k={k}\"); end); "
        "  Buss.Feed.Raise(9); sub.Dispose(); return 0; "
        "end Main; end Cs;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("k=4") != std::string::npos);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSubscribeNeedle));
}

BEZEL_TEST(lambda_own_local_not_a_capture_6284,
           subscribe_outer_capture_in_interpolation_still_rejected_spec_6_5) {
    auto r = runAot(
        "module M; import System.Threading; "
        "class Buss public static owned readonly Feed: SharedEventSource<int> := new(); end Buss; "
        "class Cs public static method Main(): int "
        "  var n := 3; "
        "  var sub := Buss.Feed.Subscribe(lambda(x: int) => do var k := 4; "
        "Console.WriteLine($\"k={k}{n}\"); end); "
        "  Buss.Feed.Raise(9); sub.Dispose(); return 0; "
        "end Main; end Cs;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSubscribeNeedle));
}

// ---- `static event +=` (#4920) ----------------------------------------------

BEZEL_TEST(lambda_own_local_not_a_capture_6284,
           static_event_own_local_plain_accepted_spec_28_14) {
    auto r = runAot(
        "module M; "
        "class C public static event Ev: int; "
        "public static method Main(): int "
        "  Ev += lambda(x: int) => do var k := 4; Console.WriteLine(k); end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kStaticEventNeedle));
}

BEZEL_TEST(lambda_own_local_not_a_capture_6284,
           static_event_own_local_interpolated_accepted_spec_28_14) {
    auto r = runAot(
        "module M; "
        "class C public static event Ev: int; "
        "public static method Main(): int "
        "  Ev += lambda(x: int) => do var k := 4; Console.WriteLine($\"k={k}\"); end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kStaticEventNeedle));
}

BEZEL_TEST(lambda_own_local_not_a_capture_6284,
           static_event_outer_capture_in_interpolation_still_rejected_spec_28_14) {
    auto r = runAot(
        "module M; "
        "class C public static event Ev: int; "
        "public static method Main(): int "
        "  var n := 3; "
        "  Ev += lambda(x: int) => do var k := 4; Console.WriteLine($\"k={k}{n}\"); end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kStaticEventNeedle));
}

// ---- extern callback (§32.8 / #4671) -----------------------------------------

BEZEL_TEST(lambda_own_local_not_a_capture_6284,
           extern_callback_own_local_plain_unchanged_pending_6287_spec_32_8) {
    // Base (7f79b510) REJECTS this with the extern capture-free text. Kept
    // until #6287 (extern callback invocation segfaults): the own-local
    // exclusion is not applied to a plain extern callback.
    auto r = runAot(
        "module Conf; "
        "extern \"libc\" method qsort(basePtr: IntPtr, nmemb: UIntPtr, "
        "size: UIntPtr, compar: (IntPtr, IntPtr) -> int): (); end; "
        "public static class IntSorter "
        "  public static method S(arrayPtr: IntPtr, count: UIntPtr, sizeOfInt: UIntPtr) "
        "    qsort(arrayPtr, count, sizeOfInt, "
        "lambda(a: IntPtr, b: IntPtr): int => do var k := 0; return k; end); "
        "  end S; "
        "  public static method Main(): int return 0; end Main; "
        "end IntSorter;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kExternNeedle));
    BEZEL_EXPECT_TRUE(rejectedBy(r, "(§32.8)"));
}

BEZEL_TEST(lambda_own_local_not_a_capture_6284,
           extern_callback_own_local_interpolated_unchanged_pending_6287_spec_32_8) {
    // Base ACCEPTED this (interpolations were not walked). Kept so the
    // extern interpolation does not start rejecting own locals.
    auto r = runAot(
        "module Conf; "
        "extern \"libc\" method qsort(basePtr: IntPtr, nmemb: UIntPtr, "
        "size: UIntPtr, compar: (IntPtr, IntPtr) -> int): (); end; "
        "public static class IntSorter "
        "  public static method S(arrayPtr: IntPtr, count: UIntPtr, sizeOfInt: UIntPtr) "
        "    qsort(arrayPtr, count, sizeOfInt, "
        "lambda(a: IntPtr, b: IntPtr): int => do var k := 0; "
        "return $\"{k}\".Length; end); "
        "  end S; "
        "  public static method Main(): int return 0; end Main; "
        "end IntSorter;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kExternNeedle));
}

BEZEL_TEST(lambda_own_local_not_a_capture_6284,
           extern_callback_outer_capture_in_interpolation_still_rejected_spec_32_8) {
    auto r = runAot(
        "module Conf; "
        "extern \"libc\" method qsort(basePtr: IntPtr, nmemb: UIntPtr, "
        "size: UIntPtr, compar: (IntPtr, IntPtr) -> int): (); end; "
        "public static class IntSorter "
        "  public static method S(arrayPtr: IntPtr, count: UIntPtr, sizeOfInt: UIntPtr, "
        "bias: int) "
        "    qsort(arrayPtr, count, sizeOfInt, "
        "lambda(a: IntPtr, b: IntPtr): int => do var k := 0; "
        "return $\"{k}{bias}\".Length; end); "
        "  end S; "
        "  public static method Main(): int return 0; end Main; "
        "end IntSorter;");
    BEZEL_EXPECT_TRUE(rejectedBy(r, kExternNeedle));
}
