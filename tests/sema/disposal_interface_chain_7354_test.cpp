// Regression tests for issue #7354 (shape 3) and its filed twin #7394 —
// a `record` that lists `IAsyncDisposable` COMPILED, while a `record` that
// lists `IDisposable` was REJECTED.
//
// THE GOVERNING TEXT.
//
//   spec §28.12 (v0.345), Bezel_Language_Spec.md:7475 — "`IAsyncDisposable`
//   extends `IDisposable`, so it conforms to `IDisposable` under §14.2's rule
//   3 … and only a class implements it — a record, enum or choice that lists
//   it, or lists an interface extending it, is a compile error at the
//   declaration, as no value kind implements `IDisposable`."
//
//   spec §9.7 (v0.345), Bezel_Language_Spec.md:1409 — "Records cannot have
//   destructors, `owned` fields, or implement `IDisposable` — directly or
//   through an interface extending it, `IAsyncDisposable` among them."
//
//   spec §8.7 (v0.345), Bezel_Language_Spec.md:1180 — "`Dispose()` is
//   class-only. The compiler generates `IDisposable` on every non-static
//   class. Static classes, records, enums, choices, primitives, and inline
//   arrays do not implement `IDisposable`."
//
// THE DEFECT.
//
//   The record-declaration check was NAME-BASED: it matched the spelled name
//   `IDisposable` and nothing else. So `record R: IAsyncDisposable` compiled
//   while `record R: IDisposable` was rejected — the asymmetry the issue
//   names — and a record listing a USER-DEFINED interface that extends
//   `IDisposable` (or `IAsyncDisposable`) compiled too.
//
//   THE RULE IS ABOUT THE INTERFACE CHAIN, NOT ABOUT A NAME. Adding
//   `"IAsyncDisposable"` to the name test would have fixed the asymmetry and
//   left the chain broken. The rows below pin the CHAIN: the user-interface
//   rows are the ones a name-based test cannot reach.
//
// THE ORACLE.
//
//   A rejection is pinned by the DIAGNOSTIC'S TEXT at a REAL LOCATION — never
//   by "the program did not compile" and never by an exit code.
//
// THE CONTROLS.
//
//   A CLASS implementing `IAsyncDisposable` (or a user interface extending
//   `IDisposable`) is still ADMITTED; a record listing a NON-disposal
//   interface is still admitted; and §28.12's other three shapes — the
//   `where T: IDisposable` bound admitting `IAsyncDisposable` and
//   `IAsyncEnumerator<T>`, the synchronous `Dispose()` on a value held at
//   `IAsyncDisposable`, and `using var` over an `IAsyncDisposable` — are
//   unchanged.
//
// Cite: language spec §28.12 / §9.7 / §8.7 / §14.2 rule 3 (v0.345);
// stdlib §2.18, §4.12, §4.13 for the built-in extension chain.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot, runAotChecked
#include "test_harness.hpp"

#include <string>
#include <vector>

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Builds a program from `lines`; the 1-based line number of `lines[i]` is
// i + 1, so a row can name the line it expects the diagnostic on without
// counting newlines by hand.
std::string prog(const std::vector<std::string>& lines) {
    std::string s;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        s += lines[i];
        s += "\n";
    }
    return s;
}

// The oracle: the program was REJECTED AT COMPILE TIME, and the diagnostic
// carrying the expected TEXT sits at the expected REAL LOCATION.
bool rejectedAt(const std::string& program, int line, int col,
                const char* text) {
    JitResult r = runAot(program);
    if (!bezel::test::failedAtCompileTimeWithRealLocation(r)) return false;
    const std::string needle =
        ":" + std::to_string(line) + ":" + std::to_string(col) + ": ";
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        std::size_t pos = 0;
        while ((pos = d.message.find(needle, pos)) != std::string::npos) {
            const std::size_t start = pos + needle.size();
            const std::size_t eol = d.message.find('\n', start);
            const std::string one =
                d.message.substr(start, eol == std::string::npos
                                          ? std::string::npos
                                          : eol - start);
            if (one.find(text) != std::string::npos) return true;
            pos += needle.size();
        }
    }
    return false;
}

// The control oracle: the program compiles, links, runs, exits 0 and prints
// exactly `expectedStdout`.
void expectRunsClean(const std::string& program,
                     const std::string& expectedStdout) {
    JitResult r = runAotChecked(program);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(bezel::test::diagnosticMessageContains(r, "sanitizer fired"));
    BEZEL_EXPECT_FALSE(bezel::test::diagnosticMessageContains(r, "AddressSanitizer"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expectedStdout);
}

// The control oracle for a program that must COMPILE (it need not run).
void expectCompiles(const std::string& program) {
    JitResult r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.12's chain, named in the diagnostic.
const char* kAsyncDisposableChain =
    "`IAsyncDisposable` extends `IDisposable`";

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// REJECT rows — §28.12 / §9.7 (v0.345).
// ═══════════════════════════════════════════════════════════════════════════

// The pre-fix behavior, pinned: `record R: IDisposable` was ALREADY rejected
// (the name-based check caught it) with this exact wording. The wording is
// unchanged by this fix — a diagnostic-text rename would be a behavior flip.
BEZEL_TEST(disposal_interface_chain_7354,
           record_listing_idisposable_is_rejected_spec_9_7) {
    const std::string p = prog({
        "module Conf;",
        "public record R: IDisposable public _x: int := 0; end R;",
    });
    BEZEL_EXPECT_TRUE(
        rejectedAt(p, 2, 8, "record `R` may not implement `IDisposable` (§9.7)"));
}

// THE SHAPE THE ISSUE NAMES: `record R: IAsyncDisposable` compiled before
// the fix, while `record R: IDisposable` was rejected — the asymmetry.
BEZEL_TEST(disposal_interface_chain_7354,
           record_listing_i_async_disposable_is_rejected_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public record R: IAsyncDisposable public _x: int := 0; end R;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 2, 8,
        "record `R` may not implement `IAsyncDisposable` — "
        "`IAsyncDisposable` extends `IDisposable` (§9.7, §28.12)"));
}

// THE CHAIN ROW — the one a name-based test CANNOT reach: a record listing a
// USER-DEFINED interface that extends `IDisposable`. §28.12: "a record, enum
// or choice that lists it, or lists an interface extending it, is a compile
// error at the declaration."
BEZEL_TEST(disposal_interface_chain_7354,
           record_listing_a_user_interface_extending_idisposable_is_rejected_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public interface MyDisp: IDisposable end MyDisp;",
        "public record R: MyDisp public _x: int := 0; end R;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 3, 8,
        "record `R` may not implement `MyDisp` — `MyDisp` extends "
        "`IDisposable` (§9.7, §28.12)"));
}

// THE SAME CHAIN, one link deeper: a user interface that extends
// `IAsyncDisposable` (which extends `IDisposable`).
BEZEL_TEST(disposal_interface_chain_7354,
           record_listing_a_user_interface_extending_i_async_disposable_is_rejected_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public interface MyDisp2: IAsyncDisposable end MyDisp2;",
        "public record R: MyDisp2 public _x: int := 0; end R;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 3, 8,
        "record `R` may not implement `MyDisp2` — `MyDisp2` extends "
        "`IAsyncDisposable`, which extends `IDisposable` (§9.7, §28.12)"));
}

// §28.12 names the enum in the same declaration-site ban: "a record, enum or
// choice that lists it, or lists an interface extending it, is a compile
// error at the declaration". MEASURED on the pre-fix tip: this compiled.
BEZEL_TEST(disposal_interface_chain_7354,
           enum_listing_i_async_disposable_is_rejected_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public enum E: IAsyncDisposable A, B; end E;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 2, 8,
        "enum `E` may not implement `IAsyncDisposable` — "
        "`IAsyncDisposable` extends `IDisposable` (§8.7, §28.12)"));
}

// §28.12 names the choice too ("a record, enum or choice that lists it").
// MEASURED on the pre-fix tip: this compiled.
BEZEL_TEST(disposal_interface_chain_7354,
           choice_listing_i_async_disposable_is_rejected_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public choice C: IAsyncDisposable A, B; end C;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 2, 8,
        "choice `C` may not implement `IAsyncDisposable` — "
        "`IAsyncDisposable` extends `IDisposable` (§8.7, §28.12)"));
}

// A built-in interface that extends `IDisposable` one link further down the
// chain: `IAsyncEnumerator<T>` extends `IAsyncDisposable` (stdlib §4.12).
BEZEL_TEST(disposal_interface_chain_7354,
           record_listing_i_async_enumerator_is_rejected_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public record R: IAsyncEnumerator<int> public _x: int := 0; end R;",
    });
    BEZEL_EXPECT_TRUE(rejectedAt(
        p, 2, 8,
        "record `R` may not implement `IAsyncEnumerator` — `IAsyncEnumerator` "
        "extends `IAsyncDisposable`, which extends `IDisposable` "
        "(§9.7, §28.12)"));
}

// ═══════════════════════════════════════════════════════════════════════════
// CONTROL rows — green on BOTH arms; they prove the fix widened nothing.
// ═══════════════════════════════════════════════════════════════════════════

// §28.12: "only a class implements it". A CLASS implementing
// `IAsyncDisposable` is still ADMITTED.
BEZEL_TEST(disposal_interface_chain_7354,
           class_listing_i_async_disposable_is_still_admitted_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public class Res: IAsyncDisposable",
        "  public async method DisposeAsync(): Task",
        "  end DisposeAsync;",
        "end Res;",
        "public class Program",
        "  public static method Main()",
        "    Console.WriteLine(\"hi\");",
        "  end Main;",
        "end Program;",
    });
    expectCompiles(p);
}

// A CLASS implementing a USER interface that extends `IDisposable` is still
// admitted — the chain rule bars the VALUE kinds, not the classes.
BEZEL_TEST(disposal_interface_chain_7354,
           class_listing_a_user_interface_extending_idisposable_is_still_admitted_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public interface MyDisp: IDisposable end MyDisp;",
        "public class C: MyDisp end C;",
        "public class Program",
        "  public static method Main()",
        "    Console.WriteLine(\"hi\");",
        "  end Main;",
        "end Program;",
    });
    expectCompiles(p);
}

// A record listing a NON-disposal interface is still admitted — the check
// keys on the disposal CHAIN, not on "the record lists an interface".
BEZEL_TEST(disposal_interface_chain_7354,
           record_listing_a_non_disposal_interface_is_still_admitted_spec_9_7) {
    const std::string p = prog({
        "module Conf;",
        "public interface IFoo method Bar(): int; end IFoo;",
        "public record R: IFoo",
        "  public method Bar(): int => 7;",
        "end R;",
        "public class Program",
        "  public static method Main()",
        "    Console.WriteLine(\"hi\");",
        "  end Main;",
        "end Program;",
    });
    expectCompiles(p);
}

// §28.12 / §14.2 rule 3: "`where T: IDisposable` admits it and every
// interface extending it (`IAsyncEnumerator<T>` among them)". The bound
// still admits `IAsyncDisposable` …
BEZEL_TEST(disposal_interface_chain_7354,
           where_idisposable_still_admits_i_async_disposable_spec_14_2) {
    const std::string p = prog({
        "module Conf;",
        "public class Box<T> where T: IDisposable end Box;",
        "public class Program",
        "  public static method Main()",
        "    var b := new Box<IAsyncDisposable>();",
        "    Console.WriteLine(\"hi\");",
        "  end Main;",
        "end Program;",
    });
    expectCompiles(p);
}

// … and `IAsyncEnumerator<T>` (stdlib §4.12: it extends
// `IAsyncDisposable`).
BEZEL_TEST(disposal_interface_chain_7354,
           where_idisposable_still_admits_i_async_enumerator_spec_14_2) {
    const std::string p = prog({
        "module Conf;",
        "public class Box<T> where T: IDisposable end Box;",
        "public class Program",
        "  public static method Main()",
        "    var b := new Box<IAsyncEnumerator<int>>();",
        "    Console.WriteLine(\"hi\");",
        "  end Main;",
        "end Program;",
    });
    expectCompiles(p);
}

// §28.12 step 1: "for plain `using` (or a synchronous `Dispose()` call) on a
// class that implements both, the destructor body runs synchronously". A
// value held at `IAsyncDisposable` still has the synchronous `Dispose()`.
BEZEL_TEST(disposal_interface_chain_7354,
           dispose_on_an_i_async_disposable_value_runs_the_destructor_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public class AsyncRes: IAsyncDisposable",
        "  destructor",
        "    Console.WriteLine(\"dtor\");",
        "  end destructor;",
        "  public async method DisposeAsync(): Task",
        "    Console.WriteLine(\"async\");",
        "  end DisposeAsync;",
        "end AsyncRes;",
        "public class Program",
        "  public static method Main()",
        "    var r := new AsyncRes();",
        "    var d: IAsyncDisposable := move r;",
        "    d.Dispose();",
        "    Console.WriteLine(\"done\");",
        "  end Main;",
        "end Program;",
    });
    expectRunsClean(p, "dtor\ndone\n");
}

// §28.12: `using var` over an `IAsyncDisposable` takes the SYNCHRONOUS
// disposal path (the object has no `DisposeAsync` obligation on that path).
BEZEL_TEST(disposal_interface_chain_7354,
           using_var_over_an_i_async_disposable_takes_the_sync_path_spec_28_12) {
    const std::string p = prog({
        "module Conf;",
        "public class AsyncRes: IAsyncDisposable",
        "  destructor",
        "    Console.WriteLine(\"dtor\");",
        "  end destructor;",
        "  public async method DisposeAsync(): Task",
        "    Console.WriteLine(\"async\");",
        "  end DisposeAsync;",
        "end AsyncRes;",
        "public class Program",
        "  public static method Main()",
        "    using var d: IAsyncDisposable := new AsyncRes();",
        "    Console.WriteLine(\"done\");",
        "  end Main;",
        "end Program;",
    });
    expectRunsClean(p, "done\ndtor\n");
}
