// Regression test for issue #9223 — the NULL-CONDITIONAL (`?.`) spelling of a
// member stdlib §7.4's `TextReader` / `TextWriter` do NOT declare was
// ACCEPTED on the built-in `Console.In` / `Console.Out` / `Console.Error`
// property read, and lowered to NOTHING.
//
// THE DEFECT. stdlib §2.1 (v0.359) declares
//
//     public static readonly property In: TextReader;
//     public static readonly property Out: TextWriter;
//     public static readonly property Error: TextWriter;
//
// and stdlib §7.4 (v0.359) declares each of those classes' member surface:
//
//     public abstract class TextReader
//       public abstract method Read(): int;
//       public abstract method ReadLine(): string?;
//       public method ReadToEnd(): string;
//       destructor;
//     end TextReader;
//
//     public abstract class TextWriter
//       public abstract method Write(value: string);
//       public abstract method Write(value: char);
//       public method Write(value: Object);
//       public method WriteLine();
//       public method WriteLine(value: string);
//       public method WriteLine(value: Object);
//       public abstract method Flush();
//       destructor;
//     end TextWriter;
//
// #9184 (stdlib §7.4, §26.5 rule 2, v0.359) refuses a member outside that
// surface on the `.` spelling, through `checkConsoleStreamMemberAccess`. The
// `?.` spelling is a DIFFERENT AST node (`ast::NullCondMemberExpr`) with its
// own resolver (`resolveNullCondMember`), and that resolver reaches neither
// the check nor the ordinary member lookup's receiver type: the console
// stream property read is UNTYPED outside a call-argument position (§2.1's
// rule — `consoleStreamArgumentType`), so the null-conditional resolver had
// NO receiver type to resolve against, found no member, reported NOTHING, and
// codegen lowered the whole access to NOTHING.
//
// THE RULE. §26.5 rule 2 (v0.359): "Where neither rule 1 nor this search
// supplies the name, nothing supplies it: the access names no member and is
// a compile error at the name, the diagnostic naming the receiver's static
// type." … "The outcome is the same for every member access on a value, not
// only a call — a property or field read or write, and a method reference,
// which resolves its member exactly as the call does (§27.6) — **with a `?.`
// or postfix-`!` receiver read at its underlying non-nullable type (§7.5,
// §7.8).** §7.5 (v0.359): "`x?.M` returns null if `x` is null; otherwise it
// performs the access." — the `?.` spelling performs the SAME access the `.`
// spelling does, so the two spellings of ONE member may not disagree about
// whether it exists.
//
// The receiver's static type is the stdlib-DECLARED one — `TextReader` for
// `Console.In`, `TextWriter` for `Console.Out` / `Console.Error` — so the
// refusal is the ORDINARY no-member diagnostic, the one sema's
// `knownCompleteUserType` catch-all draws ("no member `M` on type `T`
// (§11.3, §12.1)") — the SAME sentence the `.` spelling draws.
//
// ORACLE. `runAot` — the real AOT path (fork-isolated `Driver::compileFile`
// + clang link + native exec). The refusal rows assert the program's
// REFUSAL and the EXACT diagnostic text, so every one of them is a POSITIVE
// observable: nothing here asserts the absence of a diagnostic.
//
// EVERY REFUSAL ROW IS RED ON THE BASE. On r30-c1 the program compiled, ran,
// printed `after` and exited 0 — so `r.ran` was TRUE and there was no
// diagnostic to match. MEASURED, per row, with `--no-results-cache`; see the
// commit body.
//
// THE TWO CONTROL ROWS are green on the base BY DESIGN (they are the
// must-still-admit twins), and they are the suite's only base-green rows:
//   * `null_conditional_declared_member_on_console_out_is_admitted_26_5` —
//     `Console.Out?.WriteLine("x")` — a member stdlib §7.4 DOES declare, so
//     §26.5 rule 2 supplies it and the access is admitted. It is red under
//     the WRONG FIX "refuse every `?.` access on a console stream receiver".
//   * `dot_spelling_unknown_member_on_console_out_is_refused_26_5` — the `.`
//     spelling, `Console.Out.Bogus()`, which #9184 already refuses. It is
//     the twin that proves the two spellings of ONE member now AGREE, and it
//     is red under the WRONG FIX "the `?.` refusal replaces the `.` one"
//     (i.e. a fix that drops the `.`-spelling check instead of adding the
//     `?.` one).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// The issue's literal repro — `Console.Out?.Bogus();` and `Console.In?.Peek();`
// in STATEMENT position.
const char* kIssueRepro = R"BZL(
module P;
public class Program
  public static method Main()
    Console.Out?.Bogus();
    Console.In?.Peek();
    Console.WriteLine("after");
  end Main;
end Program;
)BZL";

// The `Console.In` twin: a member stdlib §7.4's `TextReader` does not
// declare. `Peek` is a `Stack` / `Queue` member (stdlib §4.11 / §4.12), and
// `TextReader` declares no base type.
const char* kInStatement = R"BZL(
module P;
public class Program
  public static method Main()
    Console.In?.Peek();
    Console.WriteLine("after");
  end Main;
end Program;
)BZL";

// The `Console.Error` twin: the other `TextWriter` property read.
const char* kErrorStatement = R"BZL(
module P;
public class Program
  public static method Main()
    Console.Error?.Bogus();
    Console.WriteLine("after");
  end Main;
end Program;
)BZL";

// The same access in VALUE-DEMANDING position: `var x := Console.Out?.Bogus;`.
// §26.5 rule 2: "The outcome is the same for every member access on a value,
// not only a call — a property or field read or write, and a method
// reference." On the base this position drew only the `internal:` #2434
// degrade note — the member access itself was still accepted.
const char* kOutValuePosition = R"BZL(
module P;
public class Program
  public static method Main()
    var x := Console.Out?.Bogus;
    Console.WriteLine("after");
  end Main;
end Program;
)BZL";

// The MUST-STILL-ADMIT twin: a member stdlib §7.4's `TextWriter` DOES
// declare (`WriteLine(value: string)`), reached through the `?.` spelling.
const char* kDeclaredMember = R"BZL(
module P;
public class Program
  public static method Main()
    Console.Out?.WriteLine("x");
    Console.WriteLine("after");
  end Main;
end Program;
)BZL";

// The `.`-spelling twin of the issue's repro: #9184 already refuses it, so
// the two spellings of ONE member must AGREE.
const char* kDotSpelling = R"BZL(
module P;
public class Program
  public static method Main()
    Console.Out.Bogus();
    Console.WriteLine("after");
  end Main;
end Program;
)BZL";

// §26.5 rule 2's ordinary no-member diagnostic, with the receiver's
// static type — the stdlib-DECLARED one.
std::string noMember(const std::string& typeName, const std::string& member) {
    return "no member `" + member + "` on type `" + typeName +
           "` (§11.3, §12.1)";
}

// The refusal row's assertion: the program is REFUSED at compile time, with
// the ordinary no-member diagnostic naming the receiver's static type.
void expectRefused(const char* src, const std::string& typeName,
                   const std::string& member) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, noMember(typeName, member).c_str()));
}

}  // namespace

// ── The issue's literal repro ───────────────────────────────────────────────

BEZEL_TEST(console_stream_nullcond_undeclared_member_9223,
           null_conditional_unknown_member_on_console_out_is_refused_26_5) {
    expectRefused(kIssueRepro, "TextWriter", "Bogus");
}

// ── `Console.In` — a `TextReader` (stdlib §2.1 / §7.4) ─────────────────────

BEZEL_TEST(console_stream_nullcond_undeclared_member_9223,
           null_conditional_unknown_member_on_console_in_is_refused_26_5) {
    expectRefused(kInStatement, "TextReader", "Peek");
}

// ── `Console.Error` — a `TextWriter` (stdlib §2.1 / §7.4) ───────────────────

BEZEL_TEST(console_stream_nullcond_undeclared_member_9223,
           null_conditional_unknown_member_on_console_error_is_refused_26_5) {
    expectRefused(kErrorStatement, "TextWriter", "Bogus");
}

// ── The value-demanding position — the SAME access (§26.5 rule 2) ──────────

BEZEL_TEST(console_stream_nullcond_undeclared_member_9223,
           null_conditional_property_read_on_console_out_is_refused_26_5) {
    expectRefused(kOutValuePosition, "TextWriter", "Bogus");
}

// ── CONTROL 1 (base-green): a DECLARED member via `?.` stays ADMITTED ──────
//
// The observable asserted here is ADMISSION, not the write's effect: the
// program COMPILES, RUNS and exits 0, and the statement after the `?.`
// access is reached. §26.5 rule 2 supplies the name (`WriteLine(value:
// string)` IS on stdlib §7.4's `TextWriter` surface), so the access is
// admitted and must NOT be refused.
//
// The `?.` spelling's ACCESS itself is a separate, pre-existing gap — the
// console stream property read is untyped outside a call-argument position,
// so the null-conditional resolver finds no member and codegen lowers the
// access to NOTHING (`Console.Out?.WriteLine("x")` prints nothing where the
// `.` spelling prints `x`). That is NOT what this issue is about, and this
// row deliberately does not assert the write's effect either way: it pins
// the refusal this fix adds, nothing more.
BEZEL_TEST(console_stream_nullcond_undeclared_member_9223,
           null_conditional_declared_member_on_console_out_is_admitted_26_5) {
    auto r = runAot(kDeclaredMember);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("after") != std::string::npos);
}

// ── CONTROL 2 (base-green): the `.` spelling AGREES with the `?.` one ──────

BEZEL_TEST(console_stream_nullcond_undeclared_member_9223,
           dot_spelling_unknown_member_on_console_out_is_refused_26_5) {
    expectRefused(kDotSpelling, "TextWriter", "Bogus");
}
