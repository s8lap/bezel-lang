// Spec-derived regression test for the SEMANTIC half of issue #8027.
//
// stdlib §3.2 (v0.350) declares, verbatim:
//
//   public abstract method GetBytes(s: string, destination: Span<byte>): int;
//                                                                           // returns bytes written;
//                                                                           //   IndexOutOfRangeException if too short
//
// so the DESTINATION is a WRITABLE `Span<byte>`: stdlib §4.14's `Span<T>` over
// an `OwningBuffer<T>` or §4.15's `ArraySpan<T>` over an `Array<T>`. stdlib
// §4.14's `ReadOnlySpan<T>` and §4.15's `ReadOnlyArraySpan<T>` are READ-ONLY
// VIEWS of the very memory the writable span writes.
//
// §5.1's conversion table carries NO span-to-span conversion at all, so a
// `ReadOnlySpan<byte>` is not admissible at a `Span<byte>` parameter on the
// spec's own terms — the correct answer is a REFUSAL, not a write.
//
// SCOPE — READ THIS BEFORE MERGING. Issue #8027 is CLOSED by the codegen
// lane's commit `3086ff5c` ("`Encoding.GetBytes` refuses a READ-ONLY span
// destination"), which puts a compile-time refusal in the CODEGEN arm
// (`src/codegen/LlvmBackend_Calls2.cpp`, `tryLowerEncodingCall`). THIS FILE
// pins only the SEMANTIC half — §5.1's ADMISSION gate
// (`ExprTC::builtinArgAdmitsKind` → `isWritableByteSpanType`, in
// `src/sema/ExprTypecheck_Calls.cpp`) — so that the refusal is a §5.1
// DIAGNOSTIC made where the DECLARED type is checked, instead of a
// lowering-stage error. The two compose: whichever refuses first, the
// program is refused. This commit does NOT name #8027.
//
// THE ORACLE is the diagnostic text plus a real source location
// (`failedAtCompileTimeWithRealLocation`), never the absence of a diagnostic;
// the CONTROL rows assert the exit code and the stdout.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// A rejection row: the program must be REJECTED at compile time, with the
// named diagnostic, at a real source location.
void expectRejected(const std::string& body, const std::string& needle) {
    auto r = runAot(body);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// A control row: the program must compile, run clean under the strict AOT
// oracle, exit 0, and print exactly `out`.
void expectStdout(const std::string& body, const std::string& out) {
    auto r = runAotChecked(body);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

std::string prog(const std::string& body) {
    return "module M;\n"
           "import System.Text;\n"
           "import System.Collections.Generic;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// The §3.2 destination rule, as the diagnostic spells it.
const std::string kDestDiag =
    "`Encoding.GetBytes` expects a `Span<byte>` destination (stdlib \xC2\xA7""3.2)";

// The §5.1 REASON — the codegen arm cannot state it (it is a LOWERING
// refusal, not a conversion one), so this is what makes the row RED on the
// base: there the program compiled clean and RAN.
const std::string kSec51 = "\xC2\xA7""5.1";

}  // namespace

// ── the defect: a READ-ONLY span view is NOT a writable destination ─────────

// stdlib §4.14's `ReadOnlySpan<T>` over an `OwningBuffer<T>`. On the base this
// compiled CLEAN and RAN (the call returned the byte count and the encoded
// bytes were written into the buffer behind the read-only view), so the row is
// RED on the base and GREEN with the fix.
BEZEL_TEST(encoding_getbytes_readonly_destination_admission_8027,
           readonly_span_destination_is_refused) {
    expectRejected(prog("using var buf := new OwningBuffer<byte>(8); "
                        "var n := Encoding.UTF8.GetBytes(\"ab\", buf.AsReadOnlySpan()); "
                        "Console.WriteLine(n);"),
                   kDestDiag);
}

// The offending type is named, so the diagnostic says WHICH argument was
// refused and not merely that something was — and it carries the §5.1
// REASON, which the codegen arm cannot state.
BEZEL_TEST(encoding_getbytes_readonly_destination_admission_8027,
           readonly_span_destination_names_the_type) {
    auto r = runAot(prog("using var buf := new OwningBuffer<byte>(8); "
                         "var n := Encoding.UTF8.GetBytes(\"ab\", buf.AsReadOnlySpan()); "
                         "Console.WriteLine(n);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, "got `ReadOnlySpan<byte>`"));
    BEZEL_EXPECT_TRUE(hasError(r, kSec51));
}

// The §22.5 named spelling — the same destination, written by name.
BEZEL_TEST(encoding_getbytes_readonly_destination_admission_8027,
           named_readonly_span_destination_is_refused) {
    expectRejected(prog("using var buf := new OwningBuffer<byte>(8); "
                        "var n := Encoding.UTF8.GetBytes(s := \"ab\", "
                        "destination := buf.AsReadOnlySpan()); "
                        "Console.WriteLine(n);"),
                   kDestDiag);
}

// stdlib §4.15's `ReadOnlyArraySpan<T>` over an `Array<T>` — the same defect,
// the same refusal.
BEZEL_TEST(encoding_getbytes_readonly_destination_admission_8027,
           readonly_array_span_destination_is_refused) {
    expectRejected(prog("using var arr := new Array<byte>(8, 0); "
                        "var n := Encoding.UTF8.GetBytes(\"ab\", arr.AsReadOnlySpan()); "
                        "Console.WriteLine(n);"),
                   kDestDiag);
}

// ── CONTROLS: the WRITABLE destinations are still admitted and still write ──

// stdlib §4.14's `Span<T>` over an `OwningBuffer<T>` — the writable form the
// stdlib declares. It must still be admitted, and it must still WRITE the
// encoded bytes. The oracle is the exit code and the stdout.
BEZEL_TEST(encoding_getbytes_readonly_destination_admission_8027,
           writable_span_destination_still_writes) {
    expectStdout(prog("using var buf := new OwningBuffer<byte>(8); "
                      "var n := Encoding.UTF8.GetBytes(\"ab\", buf.AsSpan()); "
                      "Console.WriteLine(n); "
                      "Console.WriteLine(int(buf[0])); "
                      "Console.WriteLine(int(buf[1]));"),
                 "2\n97\n98\n");
}

// stdlib §4.15's `ArraySpan<T>` over an `Array<T>` — the other writable form.
BEZEL_TEST(encoding_getbytes_readonly_destination_admission_8027,
           writable_array_span_destination_still_writes) {
    expectStdout(prog("using var arr := new Array<byte>(8, 0); "
                      "var n := Encoding.UTF8.GetBytes(s := \"ab\", "
                      "destination := arr.AsSpan()); "
                      "Console.WriteLine(n); "
                      "Console.WriteLine(int(arr[0])); "
                      "Console.WriteLine(int(arr[1]));"),
                 "2\n97\n98\n");
}

// A READ-ONLY span is a perfectly good SOURCE: stdlib §3.2 declares
// `GetString(bytes: ReadOnlyStringSpan)`, and the byte-span views are the
// §4.14 / §4.15 spellings of the same memory. The narrowing refuses a
// read-only view only where the stdlib declares a WRITABLE destination; the
// source position is untouched.
BEZEL_TEST(encoding_getbytes_readonly_destination_admission_8027,
           readonly_span_is_still_a_valid_source) {
    expectStdout(prog("using var b := Encoding.UTF8.GetBytes(\"hi\"); "
                      "var s := Encoding.UTF8.GetString(b.AsReadOnlySpan()); "
                      "Console.WriteLine(s);"),
                 "hi\n");
}

// The ALLOCATING one-argument overload — `GetBytes(s): Array<byte>` — has no
// destination at all and must be untouched by this narrowing.
BEZEL_TEST(encoding_getbytes_readonly_destination_admission_8027,
           allocating_getbytes_is_unaffected) {
    expectStdout(prog("using var b := Encoding.UTF8.GetBytes(\"ab\"); "
                      "Console.WriteLine(b.Count);"),
                 "2\n");
}
