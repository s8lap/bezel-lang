// fix #6291 — spec §30.14 (v0.338): the capture walk visits the binding atom
// `(var x := E; body)` (BindExpr), so an outer name in its clause initializer
// or body is a capture.
//
// exprCaptures / stmtCaptures (behind isCaptureFreeEntry) had no arm for
// BindExpr, so `Domain.Spawn(lambda() => do Console.WriteLine((var x := n;
// x + 1)); end)` compiled although `n` is a borrow of the origin domain. The
// binding atom's clauses are embedded LocalDecls declared inside the entry
// lambda, so the walk runs them under inNewWalk: their own names stay
// non-captures (the entry's source range decides, as in #6284/#6285).
//
// Every Expr and Stmt node kind was checked. The remaining unwalked kinds are
// literals, `typeof`, `default`,
// `()`, `nameof` and `break`/`continue`/local type declarations, which hold no
// value reference to an outer local (nameof names one at compile time only,
// and is accepted as before). The transfer marker `owned x` (MarkedTransferExpr)
// is not walked either, but every outer-name transfer is rejected by the
// ownership pass ("cannot transfer captured owned binding ... inside a
// closure"), so no wrong acceptance was found for it; its rows below pin that
// rejection and the own-local control.
//
// Rows:
//   - Domain.Spawn: REJECT per binding-atom position (clause initializer,
//     body, nested atom, inside an if body); ACCEPT controls with own names.
//   - Domain.Spawn transfer markers: REJECT for an outer owned local in an
//     if-expression or case-expression arm; ACCEPT for an own owned local.
//   - Subscribe: REJECT for an outer capture in a binding atom; ACCEPT for an
//     own name in one.
//   - extern plain callback: REJECT for an outer capture in a binding atom.

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
const char* kExternNeedle =
    "callback argument at a plain (un-`env`-paired) extern function-typed "
    "parameter must be capture-free";
const char* kTransferCaptureNeedle =
    "cannot transfer captured owned binding";

// A Domain.Spawn program whose entry lambda body is `body`. `n` is an outer
// int local; `a` is an outer owned class local (a transfer target).
std::string spawnProgram(const std::string& body) {
    return "module T; "
           "public class Res "
           "  public Tag: int; "
           "  public constructor(t: int) Tag := t; end constructor; "
           "end Res; "
           "public static class Program "
           "  public static method Main(args: Array<string>): int "
           "    var n := 5; "
           "    var a := new Res(1); "
           "    using var u := Domain.Spawn(lambda() => do " +
           body +
           " end); "
           "    u.Wait(); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// An extern plain callback whose body is `body` (the outer `bias` is a
// parameter of the enclosing method, so it is a capture of the callback).
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

// ---- Domain.Spawn: binding atom (BindExpr) -------------------------------

BEZEL_TEST(spawn_capture_unwalked_exprs_6291,
           spawn_capture_in_bind_clause_initializer_rejected_spec_30_14) {
    auto r = runAot(spawnProgram(
        "Console.WriteLine((var x := n; x + 1));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_unwalked_exprs_6291,
           spawn_capture_in_bind_body_rejected_spec_30_14) {
    auto r = runAot(spawnProgram(
        "Console.WriteLine((var x := 1; x + n));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_unwalked_exprs_6291,
           spawn_capture_in_nested_bind_clause_rejected_spec_30_14) {
    auto r = runAot(spawnProgram(
        "Console.WriteLine((var x := (var y := n; y); x));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_unwalked_exprs_6291,
           spawn_capture_in_bind_inside_if_rejected_spec_30_14) {
    auto r = runAot(spawnProgram(
        "if true then Console.WriteLine((var x := n; x)); end;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_unwalked_exprs_6291,
           spawn_own_local_in_bind_clause_accepted_spec_30_14) {
    auto r = runAot(spawnProgram(
        "Console.WriteLine((var x := 3; x + 1));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_unwalked_exprs_6291,
           spawn_own_local_in_bind_body_accepted_spec_30_14) {
    auto r = runAot(spawnProgram(
        "var o := 2; Console.WriteLine((var x := 1; x + o));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSpawnNeedle));
}

BEZEL_TEST(spawn_capture_unwalked_exprs_6291,
           spawn_own_nested_bind_accepted_spec_30_14) {
    auto r = runAot(spawnProgram(
        "Console.WriteLine((var x := (var y := 2; y); x));"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSpawnNeedle));
}

// ---- Domain.Spawn: transfer markers (MarkedTransferExpr) -----------------
// Not walked by the capture walk, but an outer-name transfer is rejected by
// the ownership pass. These rows pin that rejection (never silent acceptance)
// and the own-local control.

BEZEL_TEST(spawn_capture_unwalked_exprs_6291,
           spawn_transfer_outer_owned_in_if_expr_rejected_spec_30_14) {
    auto r = runAot(spawnProgram(
        "var y := (if true then owned a else new Res(2)); "
        "Console.WriteLine(y.Tag);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kTransferCaptureNeedle));
}

BEZEL_TEST(spawn_capture_unwalked_exprs_6291,
           spawn_transfer_outer_owned_in_case_expr_rejected_spec_30_14) {
    auto r = runAot(spawnProgram(
        "var y := (case 1 of 1 => owned a, _ => new Res(2)); "
        "Console.WriteLine(y.Tag);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kTransferCaptureNeedle));
}

BEZEL_TEST(spawn_capture_unwalked_exprs_6291,
           spawn_transfer_own_owned_in_case_expr_accepted_spec_30_14) {
    auto r = runAot(spawnProgram(
        "var o := new Res(3); "
        "var y := (case 1 of 1 => owned o, _ => new Res(2)); "
        "Console.WriteLine(y.Tag);"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSpawnNeedle));
}

// ---- Subscribe (§6.5) -----------------------------------------------------

BEZEL_TEST(spawn_capture_unwalked_exprs_6291,
           subscribe_outer_capture_in_bind_rejected_spec_6_5) {
    auto r = runAot(
        "module M; import System.Threading; "
        "class Buss public static owned readonly Feed: SharedEventSource<int> := new(); end Buss; "
        "class Cs public static method Main(): int "
        "  var n := 3; "
        "  var sub := Buss.Feed.Subscribe(lambda(x: int) => do "
        "Console.WriteLine((var k := n; k)); end); "
        "  Buss.Feed.Raise(9); sub.Dispose(); return 0; "
        "end Main; end Cs;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kSubscribeNeedle));
}

BEZEL_TEST(spawn_capture_unwalked_exprs_6291,
           subscribe_own_local_in_bind_accepted_spec_6_5) {
    auto r = runAot(
        "module M; import System.Threading; "
        "class Buss public static owned readonly Feed: SharedEventSource<int> := new(); end Buss; "
        "class Cs public static method Main(): int "
        "  var sub := Buss.Feed.Subscribe(lambda(x: int) => do "
        "Console.WriteLine((var k := 4; k + x)); end); "
        "  Buss.Feed.Raise(9); sub.Dispose(); return 0; "
        "end Main; end Cs;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBy(r, kSubscribeNeedle));
}

// ---- extern plain callback (§32.8) ----------------------------------------

BEZEL_TEST(spawn_capture_unwalked_exprs_6291,
           extern_outer_capture_in_bind_rejected_spec_32_8) {
    auto r = runAot(externProgram(
        "return (var x := bias; x);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBy(r, kExternNeedle));
}
