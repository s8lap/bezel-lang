// Regression tests for #5295 — `Channel<T>.TryReceive(out var v)` wrote the
// received `T?` through a 4-byte out-var slot (ASan stack-buffer-overflow:
// "WRITE of size 8 ... partially overflows this variable").
//
// WHAT THE DOCUMENTS REQUIRE.  Stdlib §6.9 (v0.313): "For sendable value-typed
// T and `string`, the ownership modifiers drop out: ... `TryReceive(out item:
// T?): bool`"; stdlib §1 shape (a): the out parameter is `T?`, assigned `null`
// on the false path. Spec §22.4 (v0.313): "`out var name` declares a new
// variable inline. Its type is inferred from the method's `out` parameter
// declaration." So the inline recipient of `TryReceive` is a `T?` local and its
// stack slot must be sized for `T?` (the tagged `{i8,T}` struct for a value T,
// a pointer for `string`). Sema left the recipient UNTYPED (the #1336 builtin
// `out var` derivation had no Channel arm), so ensureOutVarSlots fell to its
// i32 default.
//
// MEASURED PRE-FIX (staging r23-c28 8a7b8842, `agent_check --probe --asan`):
// the issue's `Channel<int>` repro reports stack-buffer-overflow (WRITE of
// size 8 into [160,164)). The rows below need the ASan leg
// (BEZEL_AOT_FORCE_SANITIZE=address) to observe the overflow itself; without
// it the untyped recipient reads back wrong / empty (exact-stdout oracle).
//
// Rows: T = int, long, double, string, bool, byte (value + string shape), each
// as inline `out var`, a pre-declared `out v`, and the `out _` control; and a
// sealed class T (the `out owned` shape: `out using var`, a pre-declared
// recipient, `out _`) with a destructor counter that proves exactly-once
// disposal. 200 iterations each.
//
// NOT covered: a record T. `Channel<Record>` Send/TryReceive emits malformed IR
// independently of the out-var form (filed separately).

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// One loop: send item(i) to a Channel<T>, TryReceive it with RECV, and on the
// last iteration print the received value. `decl` is a pre-declared recipient
// (or empty), `recv` the call, `show` the printed expression.
std::string prog(const std::string& t, const std::string& sendExpr,
                 const std::string& decl, const std::string& recv,
                 const std::string& show, const std::string& pre = "") {
    return "module M; class C public static method Main(): int "
           "  var n := 0; " + pre +
           "  using var ch := new Channel<" + t + ">(); "
           "  using var tx := ch.NewSender(); "
           "  var i := 0; "
           "  while i < 200 do "
           "    tx.Send(" + sendExpr + "); " + decl +
           "    if " + recv + " then "
           "      n := n + 1; "
           "      if i = 199 then Console.WriteLine(" + show + "); end; "
           "    end; "
           "    i := i + 1; "
           "  end; "
           "  Console.WriteLine(n); "
           "  Console.WriteLine(ch.TryReceive(out _)); "
           "  return 0; end Main; end C;";
}

void expect(const std::string& src, const std::string& want) {
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

}  // namespace

// ---- the issue's own repro --------------------------------------------------

BEZEL_TEST(channel_tryreceive_out_var_slot_5295, issue_repro_int_inline) {
    expect("module M; class C public static method Main(): int "
           "  using var ch := new Channel<int>(); "
           "  using var tx := ch.NewSender(); tx.Send(7); "
           "  if ch.TryReceive(out var v) then Console.WriteLine(v); "
           "  else Console.WriteLine(\"none\"); end; "
           "  return 0; end Main; end C;",
           "7\n");
}

// ---- int ---------------------------------------------------------------------

BEZEL_TEST(channel_tryreceive_out_var_slot_5295, int_out_var) {
    expect(prog("int", "i", "", "ch.TryReceive(out var v)", "v"),
           "199\n200\nFalse\n");
}
BEZEL_TEST(channel_tryreceive_out_var_slot_5295, int_predeclared) {
    expect(prog("int", "i", "var v: int? := null; ", "ch.TryReceive(out v)",
                "v!"),
           "199\n200\nFalse\n");
}
BEZEL_TEST(channel_tryreceive_out_var_slot_5295, int_discard_control) {
    expect(prog("int", "i", "", "ch.TryReceive(out _)", "i"),
           "199\n200\nFalse\n");
}

// ---- long --------------------------------------------------------------------

BEZEL_TEST(channel_tryreceive_out_var_slot_5295, long_out_var) {
    expect(prog("long", "9876543210L + i", "", "ch.TryReceive(out var v)", "v"),
           "9876543409\n200\nFalse\n");
}
BEZEL_TEST(channel_tryreceive_out_var_slot_5295, long_predeclared) {
    expect(prog("long", "9876543210L + i", "var v: long? := null; ",
                "ch.TryReceive(out v)", "v!"),
           "9876543409\n200\nFalse\n");
}
BEZEL_TEST(channel_tryreceive_out_var_slot_5295, long_discard_control) {
    expect(prog("long", "9876543210L + i", "", "ch.TryReceive(out _)", "i"),
           "199\n200\nFalse\n");
}

// ---- double ------------------------------------------------------------------

BEZEL_TEST(channel_tryreceive_out_var_slot_5295, double_out_var) {
    expect(prog("double", "2.5 + i", "", "ch.TryReceive(out var v)", "v"),
           "201.5\n200\nFalse\n");
}
BEZEL_TEST(channel_tryreceive_out_var_slot_5295, double_predeclared) {
    expect(prog("double", "2.5 + i", "var v: double? := null; ",
                "ch.TryReceive(out v)", "v!"),
           "201.5\n200\nFalse\n");
}
BEZEL_TEST(channel_tryreceive_out_var_slot_5295, double_discard_control) {
    expect(prog("double", "2.5 + i", "", "ch.TryReceive(out _)", "i"),
           "199\n200\nFalse\n");
}

// ---- string ------------------------------------------------------------------

BEZEL_TEST(channel_tryreceive_out_var_slot_5295, string_out_var) {
    expect(prog("string", "$\"s{i}\"", "", "ch.TryReceive(out var v)", "v"),
           "s199\n200\nFalse\n");
}
BEZEL_TEST(channel_tryreceive_out_var_slot_5295, string_predeclared) {
    expect(prog("string", "$\"s{i}\"", "var v: string? := null; ",
                "ch.TryReceive(out v)", "v!"),
           "s199\n200\nFalse\n");
}
BEZEL_TEST(channel_tryreceive_out_var_slot_5295, string_discard_control) {
    expect(prog("string", "$\"s{i}\"", "", "ch.TryReceive(out _)", "i"),
           "199\n200\nFalse\n");
}

// ---- bool --------------------------------------------------------------------

BEZEL_TEST(channel_tryreceive_out_var_slot_5295, bool_out_var) {
    expect(prog("bool", "i > 100", "", "ch.TryReceive(out var v)", "v"),
           "True\n200\nFalse\n");
}
BEZEL_TEST(channel_tryreceive_out_var_slot_5295, bool_predeclared) {
    expect(prog("bool", "i > 100", "var v: bool? := null; ",
                "ch.TryReceive(out v)", "v!"),
           "True\n200\nFalse\n");
}
BEZEL_TEST(channel_tryreceive_out_var_slot_5295, bool_discard_control) {
    expect(prog("bool", "i > 100", "", "ch.TryReceive(out _)", "i"),
           "199\n200\nFalse\n");
}

// ---- byte --------------------------------------------------------------------

BEZEL_TEST(channel_tryreceive_out_var_slot_5295, byte_out_var) {
    expect(prog("byte", "by", "", "ch.TryReceive(out var v)", "v",
                "var by: byte := 200; "),
           "200\n200\nFalse\n");
}
BEZEL_TEST(channel_tryreceive_out_var_slot_5295, byte_predeclared) {
    expect(prog("byte", "by", "var v: byte? := null; ", "ch.TryReceive(out v)",
                "v!", "var by: byte := 200; "),
           "200\n200\nFalse\n");
}
BEZEL_TEST(channel_tryreceive_out_var_slot_5295, byte_discard_control) {
    expect(prog("byte", "by", "", "ch.TryReceive(out _)", "i",
                "var by: byte := 200; "),
           "199\n200\nFalse\n");
}

// ---- sealed class T (`out owned` shape) -------------------------------------

namespace {
const char* kR =
    "sealed class R "
    "  public V: int := 0; "
    "  public static count: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "  destructor R.count := R.count + 1; end destructor; "
    "end R; ";

std::string classProg(const std::string& decl, const std::string& recv,
                      const std::string& use) {
    return std::string("module M; ") + kR +
           "class C public static method Main(): int "
           "  using var ch := new Channel<R>(); "
           "  using var tx := ch.NewSender(); "
           "  var i := 0; "
           "  while i < 200 do "
           "    tx.Send(new R(i)); " + decl +
           "    if " + recv + " then " + use + " end; "
           "    i := i + 1; "
           "  end; "
           "  Console.WriteLine(R.count); "
           "  return 0; end Main; end C;";
}
}  // namespace

BEZEL_TEST(channel_tryreceive_out_var_slot_5295, class_out_using_var_disposes) {
    expect(classProg("", "ch.TryReceive(out using var a)",
                     "if i = 199 then Console.WriteLine(a!.V); end;"),
           "199\n200\n");
}
BEZEL_TEST(channel_tryreceive_out_var_slot_5295, class_named_recipient_disposes) {
    // A named (pre-declared) recipient solely owns the received element and
    // auto-drops it at scope end (§28.3): exactly one destructor run each.
    expect(classProg("var b: R? := null; ", "ch.TryReceive(out b)",
                     "if i = 199 then Console.WriteLine(b!.V); end;"),
           "199\n200\n");
}
BEZEL_TEST(channel_tryreceive_out_var_slot_5295, class_discard_control) {
    expect(classProg("", "ch.TryReceive(out _)", ""), "200\n");
}
