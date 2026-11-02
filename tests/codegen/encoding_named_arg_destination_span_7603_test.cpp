// fix #7603 (stdlib §3.2, §5.1, §22.5, v0.350) — `Encoding.GetBytes(s, destination)`
// admitted a DESTINATION that is not a span and wrote THROUGH it.
//
// THE DEFECT. stdlib §3.2 (v0.350) declares, verbatim:
//
//   public abstract method GetBytes(s: string, destination: Span<byte>): int;
//                                                                              // returns bytes written;
//                                                                              //   IndexOutOfRangeException if too short
//
// so the second parameter is a `Span<byte>`. The `Encoding` rows of the
// built-in parameter table carry NO declared type for that position (the
// `kinds` column is zero there), so SEMA admits whatever argument it is
// given, and the codegen arm for this overload — `tryLowerEncodingCall` in
// `src/codegen/LlvmBackend_Calls2.cpp` — assumed the argument IS a span and
// stored it into a `%bz.arrayspan` slot unconditionally. An argument that is
// not a span (`Array<byte>`, `OwningBuffer<byte>`, `string`) was therefore
// written through as if it were one.
//
// MEASURED on the base (ad048d70):
//   * `Encoding.UTF8.GetBytes(s := "ab" + i.ToString(), destination := an
//     Array<byte>)` compiled CLEAN and then died at run time with
//     `Unhandled exception: System.IndexOutOfRangeException` — exit 70;
//   * under ASan the same program SEGVs on a WRITE inside
//     `bz_encoding_utf8_write_bytes` — a write through an arbitrary address,
//     i.e. a memory-safety hole, not a diagnostic.
//
// The §22.5 (named arguments) surface of this member is NOT the defect and is
// pinned here as a control: the arm consumes
// `ctx_.builtinCallBinding(call)`, so `destination :=` binds to the
// `destination` PARAMETER in every spelling, and the §22.5 binder already
// refuses a name the member does not declare.
//
// Bezel syntax notes: inequality `<>` (§17.3); `byte` values print through
// `int(...)`; an uncaught runtime raise exits 70, so a rejected program is
// pinned through `runAot`'s `ran` flag and its diagnostics.

#include <string>

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// True iff some diagnostic contains `sub`.
bool hasErrorContaining(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The §3.2 destination rule the fix enforces. The wording is asserted, not
// merely "some error": a rejection that fires for an unrelated reason would
// satisfy a bare `ran == false` check.
const char* kDestDiag =
    "`Encoding.GetBytes` expects a `Span<byte>` destination (stdlib \xC2\xA7""3.2)";

}  // namespace

// ── The defect: a destination that is not a span is REFUSED, not written through ──

// The issue's own spelling: `destination := buf` where `buf` is an
// `Array<byte>`. On the base this compiled clean and died at run time
// (exit 70, `System.IndexOutOfRangeException`); under ASan it wrote through
// a garbage pointer. It must be a compile-time rejection.
BEZEL_TEST(encoding_named_arg_destination_span_7603,
           named_destination_array_is_refused_7603) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var buf := new Array<byte>(8, 0); "
        "  var n := Encoding.UTF8.GetBytes(s := \"ab\" + 1.ToString(), "
        "                             destination := buf); "
        "  Console.WriteLine(n); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kDestDiag));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "got `Array<byte>`"));
}

// The same defect in the REORDERED named spelling: the arm binds the named
// argument to its parameter (`ctx_.builtinCallBinding`), so the argument
// bound to `destination` is the one that must be a span.
BEZEL_TEST(encoding_named_arg_destination_span_7603,
           named_destination_array_reordered_is_refused_7603) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var buf := new Array<byte>(8, 0); "
        "  var n := Encoding.UTF8.GetBytes(destination := buf, "
        "                             s := \"ab\" + 1.ToString()); "
        "  Console.WriteLine(n); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kDestDiag));
}

// The POSITIONAL spelling of the same call: the defect is the destination's
// TYPE, not the spelling, so it is refused in every spelling.
BEZEL_TEST(encoding_named_arg_destination_span_7603,
           positional_destination_array_is_refused_7603) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var buf := new Array<byte>(8, 0); "
        "  var n := Encoding.UTF8.GetBytes(\"ab\" + 1.ToString(), buf); "
        "  Console.WriteLine(n); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kDestDiag));
}

// An `OwningBuffer<byte>` is a buffer, not a span: the span is
// `buf.AsSpan()`. Passing the buffer itself is the same defect.
BEZEL_TEST(encoding_named_arg_destination_span_7603,
           named_destination_owning_buffer_is_refused_7603) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<byte>(8); "
        "  var n := Encoding.UTF8.GetBytes(s := \"ab\" + 1.ToString(), "
        "                             destination := buf); "
        "  Console.WriteLine(n); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, kDestDiag));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "got `OwningBuffer<byte>`"));
}

// ── Controls: the legal programs still work, in every spelling ───────────────

// The legal named spelling (the issue's shape): the bytes land in the
// destination span and the program prints them.
BEZEL_TEST(encoding_named_arg_destination_span_7603,
           named_destination_span_writes_the_bytes_7603) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<byte>(8); "
        "  for var i in 0..<2 do "
        "    var n := Encoding.UTF8.GetBytes(s := \"ab\" + i.ToString(), "
        "                               destination := buf.AsSpan()); "
        "    Console.WriteLine(n); "
        "    Console.WriteLine(int(buf[0])); "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // "ab0" and "ab1" are three UTF-8 bytes each; the first is 'a' = 97.
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n97\n3\n97\n"));
}

// The legal POSITIONAL spelling — the control for the named-argument path.
BEZEL_TEST(encoding_named_arg_destination_span_7603,
           positional_destination_span_writes_the_bytes_7603) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var buf := new Array<byte>(8, 0); "
        "  var n := Encoding.UTF8.GetBytes(\"ab\" + 1.ToString(), buf.AsSpan()); "
        "  Console.WriteLine(n); "
        "  Console.WriteLine(int(buf[0])); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n97\n"));
}

// The legal REORDERED named spelling: the arm binds `destination` to the
// `destination` PARAMETER, so the bytes land in the buffer, not in the
// string. (The arm consumes `ctx_.builtinCallBinding(call)`.)
BEZEL_TEST(encoding_named_arg_destination_span_7603,
           named_destination_span_reordered_writes_the_bytes_7603) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var buf := new OwningBuffer<byte>(8); "
        "  var n := Encoding.UTF8.GetBytes(destination := buf.AsSpan(), "
        "                             s := \"ab\" + 1.ToString()); "
        "  Console.WriteLine(n); "
        "  Console.WriteLine(int(buf[0])); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n97\n"));
}

// The other `Encoding` members are unaffected: `GetByteCount`, the one-argument
// `GetBytes`, and `GetString` all keep working with named arguments.
BEZEL_TEST(encoding_named_arg_destination_span_7603,
           other_encoding_members_unaffected_7603) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  Console.WriteLine(Encoding.UTF8.GetByteCount(s := \"hi\")); "
        "  using var b := Encoding.UTF8.GetBytes(s := \"hi\"); "
        "  Console.WriteLine(b.Count); "
        "  using var s2 := Encoding.UTF8.GetString(bytes := b); "
        "  Console.WriteLine(s2); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n2\nhi\n"));
}

// ── Controls: the §22.5 rules on this member still hold ─────────────────────

// §22.5: a name the member does not declare is a compile error, not a
// crash and not a silent acceptance.
BEZEL_TEST(encoding_named_arg_destination_span_7603,
           named_unknown_parameter_is_refused_7603) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var buf := new Array<byte>(8, 0); "
        "  var n := Encoding.UTF8.GetBytes(src := \"ab\", destination := buf.AsSpan()); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "has no parameter named `src`"));
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "\xC2\xA7""22.5"));
}

// §22.5: every parameter that declares no default must be bound. The
// two-argument overload's `s` has no default, so naming only `destination`
// leaves it unbound and the call is refused.
BEZEL_TEST(encoding_named_arg_destination_span_7603,
           named_missing_required_parameter_is_refused_7603) {
    auto r = runAot(
        "module M; import System.Text; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var buf := new Array<byte>(8, 0); "
        "  var n := Encoding.UTF8.GetBytes(destination := buf.AsSpan()); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "\xC2\xA7""22.5"));
}
