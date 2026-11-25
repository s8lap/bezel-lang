// Regression test for issue #9184 — a member stdlib §7.4's `TextReader` /
// `TextWriter` do NOT declare was ACCEPTED on the built-in `Console.In` /
// `Console.Out` / `Console.Error` property read, and lowered to NOTHING.
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
// The property READ is left UNTYPED outside a call-argument position (§2.1's
// rule — `consoleStreamArgumentType`, src/sema/ExprTypecheck_Exprs.cpp), so
// the ordinary member lookup had NO receiver type to resolve against: the
// access found no member, reported NOTHING, and codegen lowered the whole
// access to NOTHING. The STATEMENT position carried no diagnostic of any
// severity — not even the `internal:` #2434 degrade note the
// value-demanding position got.
//
// THE RULE. §26.5 rule 2 (v0.359): "Where neither rule 1 nor this search
// supplies the name, nothing supplies it: the access names no member and is
// a compile error at the name, the diagnostic naming the receiver's static
// type." The receiver's static type is the stdlib-DECLARED one — `TextReader`
// for `Console.In`, `TextWriter` for `Console.Out` / `Console.Error` — so
// the refusal is the ORDINARY no-member diagnostic, the one sema's
// `knownCompleteUserType` catch-all draws ("no member `M` on type `T`
// (§11.3, §12.1)"). §26.5 rule 2 says the outcome is the same for every
// member access on a value, not only a call — a property or field read or
// write, and a method reference — so the check is on the ACCESS.
//
// ORACLE. `runAot` — the real AOT path (fork-isolated `Driver::compileFile`
// + clang link + native exec). Every row asserts the program's REFUSAL and
// the EXACT diagnostic text, so every row is a POSITIVE observable: nothing
// here asserts the absence of a diagnostic.
//
// EVERY ROW IS RED ON THE BASE. On r30-c1 the program compiled, ran, printed
// `after` and exited 0 — so `r.ran` was TRUE and there was no diagnostic to
// match. MEASURED, per row, with `--no-results-cache`; see the commit body.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// The issue's literal repro — `Console.In.Bogus();` in STATEMENT position.
const char* kIssueRepro = R"BZL(
module P;
public class Program
  public static method Main()
    Console.In.Bogus();
    Console.WriteLine("after");
  end Main;
end Program;
)BZL";

// The same access in VALUE-DEMANDING position: `var x := Console.In.Bogus();`.
// On the base this position drew only the `internal:` #2434 degrade note —
// the member access itself was still accepted.
const char* kIssueReproValuePosition = R"BZL(
module P;
public class Program
  public static method Main()
    var x := Console.In.Bogus();
    Console.WriteLine("after");
  end Main;
end Program;
)BZL";

// The `Console.Out` / `Console.Error` twins: the same access on the two
// `TextWriter` property reads.
const char* kOutStatement = R"BZL(
module P;
public class Program
  public static method Main()
    Console.Out.Bogus();
    Console.WriteLine("after");
  end Main;
end Program;
)BZL";

const char* kOutValuePosition = R"BZL(
module P;
public class Program
  public static method Main()
    var x := Console.Out.Bogus();
    Console.WriteLine("after");
  end Main;
end Program;
)BZL";

const char* kErrorStatement = R"BZL(
module P;
public class Program
  public static method Main()
    Console.Error.Bogus();
    Console.WriteLine("after");
  end Main;
end Program;
)BZL";

const char* kErrorValuePosition = R"BZL(
module P;
public class Program
  public static method Main()
    var x := Console.Error.Bogus();
    Console.WriteLine("after");
  end Main;
end Program;
)BZL";

// The postfix-`!` spelling: A.7's `PostfixBangExpr` is "`x!` is `x`'s own
// type" (§7.8), and §26.5 rule 2 reads the member of a postfix-`!` receiver
// "at its underlying non-nullable type" — so this is the SAME access as
// `Console.In.Bogus()`, not a second one.
const char* kBangSpelling = R"BZL(
module P;
public class Program
  public static method Main()
    Console.In!.Bogus();
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

// The row's assertion: the program is REFUSED at compile time, with the
// ordinary no-member diagnostic naming the receiver's static type.
void expectRefused(const char* src, const std::string& typeName) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, noMember(typeName, "Bogus").c_str()));
}

}  // namespace

// ── `Console.In` — a `TextReader` (stdlib §2.1 / §7.4) ─────────────────────

BEZEL_TEST(console_stream_undeclared_member_9184,
           statement_position_unknown_member_on_console_in_is_refused_26_5) {
    expectRefused(kIssueRepro, "TextReader");
}

BEZEL_TEST(console_stream_undeclared_member_9184,
           expression_position_unknown_member_on_console_in_is_refused_26_5) {
    expectRefused(kIssueReproValuePosition, "TextReader");
}

// ── `Console.Out` — a `TextWriter` (stdlib §2.1 / §7.4) ─────────────────────

BEZEL_TEST(console_stream_undeclared_member_9184,
           statement_position_unknown_member_on_console_out_is_refused_26_5) {
    expectRefused(kOutStatement, "TextWriter");
}

BEZEL_TEST(console_stream_undeclared_member_9184,
           expression_position_unknown_member_on_console_out_is_refused_26_5) {
    expectRefused(kOutValuePosition, "TextWriter");
}

// ── `Console.Error` — a `TextWriter` (stdlib §2.1 / §7.4) ───────────────────

BEZEL_TEST(console_stream_undeclared_member_9184,
           statement_position_unknown_member_on_console_error_is_refused_26_5) {
    expectRefused(kErrorStatement, "TextWriter");
}

BEZEL_TEST(console_stream_undeclared_member_9184,
           expression_position_unknown_member_on_console_error_is_refused_26_5) {
    expectRefused(kErrorValuePosition, "TextWriter");
}

// ── The postfix-`!` spelling — the SAME access (§26.5 rule 2) ──────────────

BEZEL_TEST(console_stream_undeclared_member_9184,
           bang_spelling_unknown_member_on_console_in_is_refused_26_5) {
    expectRefused(kBangSpelling, "TextReader");
}
