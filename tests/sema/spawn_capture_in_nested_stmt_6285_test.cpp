// fix #6285 — spec §30.14 (v0.338): a capture inside a nested statement of an
// entry lambda is a capture.
//
// isCaptureFreeEntry's capture walk (stmtCaptures / exprCaptures) descended
// into return, assignment, expression, local-initializer, if and do…end
// statements only. A capture inside a for, while, try/catch/finally or
// statement-case body (or in a for iterable, a range bound, a case arm guard)
// was never seen, so `Domain.Spawn(lambda() => do for var i in 1..2 do
// Console.WriteLine(n); end; end)` compiled although `n` is a borrow of the
// origin domain. The walk now descends into every statement and expression
// kind; names a body declares itself stay non-captures (the entry's source
// range decides that, as in #6284).
//
// Rows, per the issue's four callers (§30.14 Spawn, §6.5 SharedEventSource
// Subscribe, `static event +=` (§28.14), the plain extern callback (§32.8)):
//   - Domain.Spawn: one REJECT row per statement kind / receiver form, and
//     ACCEPT rows for the valid shapes (own locals, loop and catch variables,
//     case-arm bindings, static members, nested own names).
//   - Subscribe, `static event +=`: an outer capture inside a for body is
//     rejected; an own local inside a for body is accepted.
//   - extern: an outer capture inside a for or try body is rejected (base
//     accepted it, the walk did not reach it). An own local inside a for body
//     is accepted, and an own local inside an if body stays rejected: base's
//     verdict there is unchanged (see #6284's extern-parity note).

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

// A Domain.Spawn program whose entry lambda body is `body`. `n` is an outer
// local (captured when the body names it). Program has a static field and a
// static method for the static-member accept rows.
std::string spawnProgram(const std::string& body) {
    return "module T; "
           "public static class Program "
           "  public static readonly Limit: int := 2; "
           "  public static method Twice(x: int): int return x * 2; end Twice; "
           "  public static method Main(args: Array<string>): int "
           "    var n := 5; "
           "    using var u := Domain.Spawn(lambda() => do " +
           body +
           " end); "
           "    u.Wait(); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

}  // namespace

// ---- Domain.Spawn (§30.14): outer captures in nested statements -----------

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           spawn_capture_in_for_body_rejected_spec_30_14) {
    auto r = runAot(spawnProgram(
        "for var i in 1..2 do Console.WriteLine(n); end;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           spawn_capture_in_for_iterable_rejected_spec_30_14) {
    auto r = runAot(spawnProgram(
        "for var i in 1..n do Console.WriteLine(i); end;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           spawn_capture_in_while_body_rejected_spec_30_14) {
    auto r = runAot(spawnProgram(
        "var c := 0; while c < 1 do Console.WriteLine(n); c++; end;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           spawn_capture_in_try_body_rejected_spec_30_14) {
    auto r = runAot(spawnProgram(
        "try Console.WriteLine(n); catch var ex: Exception => "
        "Console.WriteLine(1); end;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           spawn_capture_in_catch_arm_rejected_spec_30_14) {
    auto r = runAot(spawnProgram(
        "try Console.WriteLine(1); catch var ex: Exception => "
        "Console.WriteLine(n); end;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           spawn_capture_in_finally_body_rejected_spec_30_14) {
    auto r = runAot(spawnProgram(
        "try Console.WriteLine(1); finally Console.WriteLine(n); end;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           spawn_capture_in_case_arm_rejected_spec_30_14) {
    auto r = runAot(spawnProgram(
        "var k := 1; case k of 1 => do Console.WriteLine(n); end, "
        "_ => do Console.WriteLine(2); end end;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           spawn_capture_in_case_arm_guard_rejected_spec_30_14) {
    auto r = runAot(spawnProgram(
        "var k := 1; case k of 1 when n > 0 => do Console.WriteLine(1); end, "
        "_ => do Console.WriteLine(2); end end;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           spawn_capture_in_if_inside_for_rejected_spec_30_14) {
    auto r = runAot(spawnProgram(
        "for var i in 1..2 do if true then Console.WriteLine(n); end; end;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           spawn_capture_in_nested_for_rejected_spec_30_14) {
    auto r = runAot(spawnProgram(
        "for var i in 1..2 do for var j in 1..2 do Console.WriteLine(n); end; end;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           spawn_capture_in_do_block_inside_for_rejected_spec_30_14) {
    auto r = runAot(spawnProgram(
        "for var i in 1..2 do do Console.WriteLine(n); end; end;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           spawn_capture_in_throw_rejected_spec_30_14) {
    auto r = runAot(spawnProgram("throw new Exception($\"{n}\");"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

// ---- Domain.Spawn (§30.14): valid shapes stay accepted ---------------------

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           spawn_own_local_in_for_body_accepted_spec_30_14) {
    auto r = runAot(spawnProgram(
        "for var i in 1..2 do var k := i; Console.WriteLine(k); end;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           spawn_loop_variable_in_nested_for_accepted_spec_30_14) {
    auto r = runAot(spawnProgram(
        "for var i in 1..2 do for var j in 1..2 do Console.WriteLine(j); end; end;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           spawn_own_local_in_while_body_accepted_spec_30_14) {
    auto r = runAot(spawnProgram(
        "var c := 0; while c < 1 do var k := 3; Console.WriteLine(k); c++; end;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           spawn_catch_variable_in_try_body_accepted_spec_30_14) {
    auto r = runAot(spawnProgram(
        "try Console.WriteLine(1); catch var ex: Exception => "
        "Console.WriteLine(ex.Message); end;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           spawn_catch_do_block_own_local_accepted_spec_30_14) {
    auto r = runAot(spawnProgram(
        "try Console.WriteLine(1); catch var ex: Exception => do var m := "
        "ex.Message; Console.WriteLine(m); end; end;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           spawn_case_arm_binding_accepted_spec_30_14) {
    auto r = runAot(spawnProgram(
        "var k := 1; case k of var j => do Console.WriteLine(j); end end;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           spawn_own_local_in_case_arm_accepted_spec_30_14) {
    auto r = runAot(spawnProgram(
        "var k := 1; case k of 1 => do var m := 4; Console.WriteLine(m); end, "
        "_ => do Console.WriteLine(2); end end;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           spawn_own_local_in_if_inside_for_accepted_spec_30_14) {
    auto r = runAot(spawnProgram(
        "for var i in 1..2 do if true then var k := 1; Console.WriteLine(k); "
        "end; end;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           spawn_static_method_and_field_in_for_accepted_spec_30_14) {
    auto r = runAot(spawnProgram(
        "for var i in 1..Program.Limit do Console.WriteLine(Program.Twice(i)); end;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSpawnNeedle));
}

// ---- Subscribe (§6.5 / #4920) and `static event +=` (§28.14) ---------------

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           subscribe_outer_capture_in_for_body_rejected_spec_6_5) {
    auto r = runAot(
        "module M; import System.Threading; "
        "class Buss public static owned readonly Feed: SharedEventSource<int> := new(); end Buss; "
        "class Cs public static method Main(): int "
        "  var n := 3; "
        "  var sub := Buss.Feed.Subscribe(lambda(x: int) => do "
        "for var i in 1..2 do Console.WriteLine(n); end; end); "
        "  Buss.Feed.Raise(9); sub.Dispose(); return 0; "
        "end Main; end Cs;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSubscribeNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           subscribe_own_local_in_for_body_accepted_spec_6_5) {
    auto r = runAot(
        "module M; import System.Threading; "
        "class Buss public static owned readonly Feed: SharedEventSource<int> := new(); end Buss; "
        "class Cs public static method Main(): int "
        "  var sub := Buss.Feed.Subscribe(lambda(x: int) => do "
        "for var i in 1..2 do var k := i; Console.WriteLine(k); end; end); "
        "  Buss.Feed.Raise(9); sub.Dispose(); return 0; "
        "end Main; end Cs;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSubscribeNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           static_event_outer_capture_in_try_body_rejected_spec_28_14) {
    auto r = runAot(
        "module M; "
        "class C public static event Ev: int; "
        "public static method Main(): int "
        "  var n := 3; "
        "  Ev += lambda(x: int) => do try Console.WriteLine(n); "
        "catch var ex: Exception => Console.WriteLine(1); end; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kStaticEventNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           static_event_own_local_in_for_body_accepted_spec_28_14) {
    auto r = runAot(
        "module M; "
        "class C public static event Ev: int; "
        "public static method Main(): int "
        "  Ev += lambda(x: int) => do for var i in 1..2 do var k := i; "
        "Console.WriteLine(k); end; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kStaticEventNeedle));
}

// ---- extern callback (§32.8 / #4671) ---------------------------------------

namespace {

// A plain extern callback whose lambda body is `body`; `bias` is an outer
// parameter of the enclosing method.
std::string externProgram(const std::string& body) {
    return "module Conf; "
           "extern \"libc\" method qsort(basePtr: IntPtr, nmemb: UIntPtr, "
           "size: UIntPtr, compar: (IntPtr, IntPtr) -> int): (); end; "
           "public static class IntSorter "
           "  public static method S(arrayPtr: IntPtr, count: UIntPtr, "
           "sizeOfInt: UIntPtr, bias: int) "
           "    qsort(arrayPtr, count, sizeOfInt, "
           "lambda(a: IntPtr, b: IntPtr): int => do " +
           body +
           " end); "
           "  end S; "
           "  public static method Main(): int return 0; end Main; "
           "end IntSorter;";
}

}  // namespace

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           extern_outer_capture_in_for_body_rejected_spec_32_8) {
    // Base accepted this (a for body was not walked). The outer `bias` is a
    // capture of the callback, so the plain extern callback rejects it.
    auto r = runAot(externProgram(
        "for var i in 1..2 do Console.WriteLine(bias); end; return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kExternNeedle));
    BEZEL_EXPECT_TRUE(rejectedBy(r, "(§32.8)"));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           extern_outer_capture_in_try_body_rejected_spec_32_8) {
    auto r = runAot(externProgram(
        "try Console.WriteLine(bias); catch var ex: Exception => "
        "Console.WriteLine(1); end; return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kExternNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           extern_own_local_in_for_body_accepted_spec_32_8) {
    // Base accepted this (the for body was not walked), so it stays accepted.
    auto r = runAot(externProgram(
        "for var i in 1..2 do var k := i; end; return 0;"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kExternNeedle));
}

BEZEL_TEST(spawn_capture_in_nested_stmt_6285,
           extern_own_local_in_if_body_still_rejected_base_parity_spec_32_8) {
    // Base REJECTED this (an if body was walked and its own local read is a
    // capture at a plain extern callback). Kept: the nested-statement walk
    // must not turn it into an accepted program.
    auto r = runAot(externProgram(
        "if true then var k := 0; Console.WriteLine(k); end; return 0;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kExternNeedle));
}
