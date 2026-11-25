// Codegen — a generic method whose RETURN TYPE is a FUNCTION TYPE over a
// NULLABLE TYPE PARAMETER (`() -> T?`) emitted malformed IR: the closure's
// LLVM return type is the TAGGED nullable CARRIER (`{i8 hasValue; T value}`)
// while its expression body yielded the BARE payload (`T`), so the `ret`
// carried an `i32` into a `{ i8, i32 }`-returning closure —
// "Function return type does not match operand type of return inst!".
// Issue #9120.
//
// Spec: §14.1 (a generic declaration is instantiated at a concrete type
// argument; the body's types are the instantiation's), §7.1 / §7.2 (a nullable
// VALUE type is the tagged `{hasValue, value}` carrier; a nullable REFERENCE
// type is a plain pointer), §22.2 (a method's declared return type is the type
// of the value it hands back). Spec revision v0.359.
//
// The defect is a COMPILE-TIME abort (the module fails LLVM verification), so
// every row below is RED on the base for the same reason: `r.ran` is false
// because the program never got as far as producing an executable.
//
// The rows exercise the instantiations the issue names — `string`, `int`,
// `string?` and a class — plus the `null`-body twin of the same lowering and
// the issue's own program verbatim. Each loops 200 times so that the
// ASan/LSan leg of `runAotChecked` sees the produced code's reference counting
// over many iterations (a closure that captures a runtime-built string,
// released once per iteration).
//
// THE CONSUMER SPELLING (why the string rows bind the result and do not
// `??`-coalesce it): a closure result typed by a DECLARED `T?` takes no +1
// mint — `mintedBareTypeParamResult` declines a nullable type parameter — so
// the result is a BORROW of the captured argument. A consumer that RELEASES
// the result (the `??` operator's left operand, or a discarded call result)
// therefore releases a reference the producer never minted and the program
// aborts with "refcount over-release (string)" (§28.9). That is a SEPARATE,
// PRE-EXISTING defect (it reproduces on the base through the block-bodied
// twin, which compiled there) and is filed separately; the rows below consume
// the closure's result AS A BORROW, so they measure this lane's lowering fix
// and not that one.
//
// The two controls are the twins the issue records as already correct: the
// NON-generic method (`() -> string?` over a plain nullable parameter) and the
// BLOCK-bodied lambda (`lambda(): T? => do return p; end`, whose `return`
// leaves through `emitReturn` and so already carried the promotion).

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when ASan / LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_TRUE(sawSummary == false);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// The issue's own repro, looped: `M<T>(p: T): () -> T?` returning
// `lambda(): T? => p`, instantiated at `string`, with the issue's own
// `f() ?? "null"` consumer. The argument is the issue's LITERAL "hi" (a
// folded literal is an RC sentinel on which release is a no-op), so this row
// is the issue's program verbatim and is not perturbed by the borrow-vs-mint
// note above. Each iteration prints one line.
const char* kIssueRepro =
    "module M; import System;\n"
    "public class Program\n"
    "  public static method M<T>(p: T): () -> T?\n"
    "    return lambda(): T? => p;\n"
    "  end M;\n"
    "  public static method Main(): int\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      var f := Program.M<string>(\"hi\");\n"
    "      Console.WriteLine(f() ?? \"null\");\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The same shape instantiated at `string` with a RUNTIME-built argument: the
// captured value is a fresh heap string (a folded literal is an RC sentinel
// and would hide a leak), three characters long, so the total is 200 * 3.
const char* kStringInst =
    "module M; import System;\n"
    "public class Program\n"
    "  public static method M<T>(p: T): () -> T?\n"
    "    return lambda(): T? => p;\n"
    "  end M;\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      var s := \"v\" + (40 + i mod 10).ToString();\n"
    "      var f := Program.M<string>(s);\n"
    "      var r := f();\n"
    "      if r <> null then tot := tot + r!.Length; end if;\n"
    "      _ := r;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The same shape instantiated at `int`: the carrier is `{i8, i32}` and the
// payload is the i32 itself, so the wrap is the tagged one (not a pointer).
const char* kIntInst =
    "module M; import System;\n"
    "public class Program\n"
    "  public static method M<T>(p: T): () -> T?\n"
    "    return lambda(): T? => p;\n"
    "  end M;\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      var f := Program.M<int>(i);\n"
    "      tot := tot + (f() ?? -1);\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The same shape instantiated at `string?`: the nullable form of a nullable
// REFERENCE is a plain pointer (§7.1), so the specialization's closure return
// type is the pointer — but the ERASED TEMPLATE of the generic method still
// declared the placeholder carrier, so the module still failed verification
// on the base.
const char* kStringQInst =
    "module M; import System;\n"
    "public class Program\n"
    "  public static method M<T>(p: T): () -> T?\n"
    "    return lambda(): T? => p;\n"
    "  end M;\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      var s := \"v\" + (40 + i mod 10).ToString();\n"
    "      var f := Program.M<string?>(s);\n"
    "      var r := f();\n"
    "      if r <> null then tot := tot + r!.Length; end if;\n"
    "      _ := r;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The same shape instantiated at a CLASS type: the nullable form of a class
// is a plain pointer (§7.1), so the closure returns the reference.
const char* kClassInst =
    "module M; import System;\n"
    "public class Box\n"
    "  public V: int;\n"
    "  public constructor(v: int) this.V := v; end constructor;\n"
    "end Box;\n"
    "public class Program\n"
    "  public static method M<T>(p: T): () -> T?\n"
    "    return lambda(): T? => p;\n"
    "  end M;\n"
    "  public static method Main(): int\n"
    "    var tot := 0;\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      var b := new Box(i);\n"
    "      var f := Program.M<Box>(b);\n"
    "      var r := f();\n"
    "      if r <> null then tot := tot + r!.V; end if;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(tot);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The `null` twin of the same lowering: the body yields a null POINTER where
// the carrier is expected — the second malformed-IR shape of the same defect
// ("ret ptr null" against "{ i8, i32 }").
const char* kNullBody =
    "module M; import System;\n"
    "public class Program\n"
    "  public static method M<T>(p: T): () -> T?\n"
    "    return lambda(): T? => null;\n"
    "  end M;\n"
    "  public static method Main(): int\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      var f := Program.M<string>(\"v\" + (40 + i mod 10).ToString());\n"
    "      var s := f() ?? \"null\";\n"
    "      _ := s.Length;\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(\"null\");\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// CONTROL 1 — the NON-generic twin the issue records as already correct:
// `() -> string?` over a plain nullable parameter. The carrier question does
// not arise (a nullable reference is a plain pointer, §7.1).
const char* kControlNonGeneric =
    "module M; import System;\n"
    "public class Program\n"
    "  public static method M(p: string?): () -> string?\n"
    "    return lambda(): string? => p;\n"
    "  end M;\n"
    "  public static method Main(): int\n"
    "    var f := Program.M(\"hi\");\n"
    "    Console.WriteLine(f() ?? \"null\");\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// CONTROL 2 — the BLOCK-bodied lambda twin: the same declared result type,
// but the body's `return` leaves through `emitReturn`, which already carried
// the promotion the expression body lacked.
const char* kControlBlockBody =
    "module M; import System;\n"
    "public class Program\n"
    "  public static method M<T>(p: T): () -> T?\n"
    "    return lambda(): T? => do return p; end;\n"
    "  end M;\n"
    "  public static method Main(): int\n"
    "    var f := Program.M<string>(\"hi\");\n"
    "    Console.WriteLine(f() ?? \"null\");\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// `n` copies of `line` — the 200-iteration rows that print one line each.
std::string repeatedLines(int n, const std::string& line) {
    std::string out;
    for (int i = 0; i < n; ++i) out += line;
    return out;
}

// The program ran to completion: it compiled, the sanitizer leg really ran
// (a toolchain SKIP fails the row instead of passing it silently), it exited
// normally with the program's own exit code and the exact expected output,
// and neither AddressSanitizer nor LeakSanitizer fired.
void expectClean(const char* src, const std::string& out) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);  // the module verified and the program ran
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    assertNoLeak(r);
}

// The PLAIN `runAot` leg of the same program: the module verifies and the
// program runs with the expected output, without the sanitizer instrumentation
// (the rows above carry the ASan/LSan leg; this one carries the plain leg).
void expectPlain(const char* src, const std::string& out) {
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// ---------------------------------------------------------------------------
// (a) THE ISSUE'S OWN PROGRAM — `() -> T?` at `string`, the issue's literal
// "hi" and the issue's `f() ?? "null"` consumer. On the base the module fails
// LLVM verification ("ret i32 %4" against "{ i8, i32 }") and no executable is
// produced, so the row is RED there.
// ---------------------------------------------------------------------------

BEZEL_TEST(ownership_generic_fn_type_nullable_type_param_9120,
           issue_repro_lowers_the_closure_carrier_spec_14_1) {
    expectClean(kIssueRepro, repeatedLines(200, "hi\n"));
}

// ---------------------------------------------------------------------------
// (a2) THE ISSUE'S OWN PROGRAM on the PLAIN `runAot` leg (no sanitizer
// instrumentation): the module verifies, the program runs, the output is the
// issue's `hi`.
// ---------------------------------------------------------------------------

BEZEL_TEST(ownership_generic_fn_type_nullable_type_param_9120,
           issue_repro_runs_on_the_plain_aot_leg_spec_22_2) {
    expectPlain(kIssueRepro, repeatedLines(200, "hi\n"));
}

// ---------------------------------------------------------------------------
// (b) The `string` instantiation with a RUNTIME-built captured value: the
// closure hands the string back and the consumer reads it.
// ---------------------------------------------------------------------------

BEZEL_TEST(ownership_generic_fn_type_nullable_type_param_9120,
           string_instantiation_lowers_the_closure_carrier_spec_14_1) {
    expectClean(kStringInst, "600\n");
}

// ---------------------------------------------------------------------------
// (c) The `int` instantiation: the carrier is `{i8, i32}` and the payload is
// the i32, so the promotion is the tagged one — a different payload shape
// from the reference instantiations.
// ---------------------------------------------------------------------------

BEZEL_TEST(ownership_generic_fn_type_nullable_type_param_9120,
           int_instantiation_lowers_the_closure_carrier_spec_14_1) {
    expectClean(kIntInst, "19900\n");
}

// ---------------------------------------------------------------------------
// (d) The `string?` instantiation: a nullable REFERENCE is a plain pointer
// (§7.1), so the specialization's closure return type is the pointer — but
// the ERASED TEMPLATE still declared the placeholder carrier, so the module
// still failed verification on the base.
// ---------------------------------------------------------------------------

BEZEL_TEST(ownership_generic_fn_type_nullable_type_param_9120,
           nullable_string_instantiation_lowers_the_closure_carrier_spec_7_1) {
    expectClean(kStringQInst, "600\n");
}

// ---------------------------------------------------------------------------
// (e) The CLASS instantiation: the nullable form of a class is a plain
// pointer (§7.1) — the closure hands the reference back.
// ---------------------------------------------------------------------------

BEZEL_TEST(ownership_generic_fn_type_nullable_type_param_9120,
           class_instantiation_lowers_the_closure_carrier_spec_7_1) {
    expectClean(kClassInst, "19900\n");
}

// ---------------------------------------------------------------------------
// (f) The `null`-body twin: the body yields a null pointer where the carrier
// is expected — the second malformed-IR shape of the same defect.
// ---------------------------------------------------------------------------

BEZEL_TEST(ownership_generic_fn_type_nullable_type_param_9120,
           null_body_lowers_the_closure_carrier_spec_7_1) {
    expectClean(kNullBody, "null\n");
}

// ---------------------------------------------------------------------------
// (g) CONTROL — the non-generic twin is unchanged (the issue records it as
// already correct).
// ---------------------------------------------------------------------------

BEZEL_TEST(ownership_generic_fn_type_nullable_type_param_9120,
           control_non_generic_twin_unchanged_spec_7_1) {
    expectClean(kControlNonGeneric, "hi\n");
}

// ---------------------------------------------------------------------------
// (h) CONTROL — the block-bodied lambda twin is unchanged (its `return` goes
// through `emitReturn`, which already carried the promotion).
// ---------------------------------------------------------------------------

BEZEL_TEST(ownership_generic_fn_type_nullable_type_param_9120,
           control_block_body_twin_unchanged_spec_22_2) {
    expectClean(kControlBlockBody, "hi\n");
}
