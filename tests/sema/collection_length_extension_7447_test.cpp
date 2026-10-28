// Spec-derived regression test for issue #7447.
//
// Spec §26.5 rule 2 (v0.345, docs/Bezel_Language_Spec.md:5715): "If the
// receiver's type declares no instance member of that name, the compiler
// searches the current module and all imported modules ... for extension
// blocks on `x`'s type, base types, or implemented interfaces."
//
// Stdlib §4.4 (v0.345) declares `Count` and `Capacity` on `Array<T>` — and
// NO `Length`; §4.11 declares `Count` (via `IReadOnlyCollection<T>`) on
// `Stack<T>` / `Queue<T>` and their Owning twins — and no `Length` either.
// The stdlib's own *Count vs Length* convention says why: "`Length` measures
// a fixed or contiguous extent ... `Count` counts the elements of a
// collection whose membership grows and shrinks ... which is why the
// resizable `Array<T>` says `Count`".
//
// THE DEFECT — a FALSE REJECT. The receiver's surface declares no member of
// the called name, so §26.5 rule 2 requires the extension blocks to be
// consulted, and the user's extension is the selected candidate. The
// compiler instead answered with the SIBLING member's name and refused the
// call:
//
//   extension Array<int> public method Length(): int => 42; end;
//   var a: Array<int> := [1,2,3];
//   Console.WriteLine(a.Length());
//   -> REFUSED: "no member `Length` on `Array<T>`: use `Count` (stdlib \S4.4)"
//
// The diagnostic named a member the receiver does not have, and it carried
// NO §-character at all: the section was spelled with a LITERAL BACKSLASH-S
// ("\S4.4"), so the message cited no §-section.
//
// THE ROWS. The value rows assert the program's exact STDOUT (the oracle is
// the RENDERED VALUE). The rejection rows assert the SPECIFIC diagnostic at
// a real source location. The CONTROL rows are load-bearing: with NO
// extension the call must STILL be refused (the head declares no `Length`),
// and the sibling `Count` must still be the member the receiver answers on.
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// A module carrying the extension block, and a `Main` that runs `body` over
// a three-element `Array<int>`.
std::string prog(const std::string& ext, const std::string& body) {
    return "module M;\n" + ext +
           "class C\n"
           "  public static method Main(): int\n"
           "    var a: Array<int> := [1,2,3];\n"
           "    " + body + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// A rejection row: the program must be REJECTED at compile time, with the
// named diagnostic, at a real source location.
void expectRejected(const std::string& ext, const std::string& body,
                     const std::string& needle) {
    auto r = runAot(prog(ext, body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// A value row: the program must compile, run, and print exactly `out`.
void expectStdout(const std::string& ext, const std::string& body,
                  const std::string& out) {
    auto r = runAot(prog(ext, body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// The `Array<T>` refusal, with the §-section the diagnostic now carries.
const char kArrayLength[] =
    "no member `Length` on `Array<T>`: use `Count` (stdlib \xC2\xA7""4.4)";

}  // namespace

// ── the DEFECT: a legal extension over a head that declares no `Length` ────

BEZEL_TEST(sema_collection_length_extension_7447,
           array_length_user_extension_is_admitted) {
    // per §26.5 rule 2 (v0.345) + stdlib §4.4
    expectStdout("extension Array<int>\n"
                 "  public method Length(): int => 42;\n"
                 "end;\n",
                 "Console.WriteLine(a.Length());",
                 "42\n");
}

// The same shape on `Stack<T>` (stdlib §4.11): the head declares `Count`,
// not `Length`, so rule 2 applies there too.
BEZEL_TEST(sema_collection_length_extension_7447,
           stack_length_user_extension_is_admitted) {
    // per §26.5 rule 2 (v0.345) + stdlib §4.11
    auto r = runAot("module M;\n"
                    "extension Stack<int>\n"
                    "  public method Length(): int => 42;\n"
                    "end;\n"
                    "class C\n"
                    "  public static method Main(): int\n"
                    "    var s := new Stack<int>();\n"
                    "    s.Push(1);\n"
                    "    Console.WriteLine(s.Length());\n"
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// ── CONTROLS ──────────────────────────────────────────────────────────────

// With NO extension the call is still REFUSED: the head declares `Count`
// and no `Length`, so there is no member to call. The refusal now carries
// its §-section (it read "(stdlib \S4.4)" — a literal backslash-S — before).
BEZEL_TEST(sema_collection_length_extension_7447,
           array_length_without_an_extension_is_still_refused) {
    // per stdlib §4.4 + §26.5 rule 2
    expectRejected("",
                   "Console.WriteLine(a.Length());",
                   kArrayLength);
}

// The sibling member the receiver DOES declare still answers: `Array<T>`
// declares `Count` (stdlib §4.4), and the Count-vs-Length convention is
// why the diagnostic points at it.
BEZEL_TEST(sema_collection_length_extension_7447,
           array_count_still_answers) {
    // per stdlib §4.4
    expectStdout("",
                 "Console.WriteLine(a.Count());",
                 "3\n");
}

// And with a same-named extension in the program, the DECLARED member still
// answers — `Array<T>` declares `Count`, so §26.5 rule 1 commits instance
// resolution for that name and the extension is unreachable.
BEZEL_TEST(sema_collection_length_extension_7447,
           array_count_declared_member_wins_over_a_same_named_extension) {
    // per §26.5 rule 1 (v0.345) + stdlib §4.4
    expectStdout("extension Array<int>\n"
                 "  public method Count(): int => 77;\n"
                 "end;\n",
                 "Console.WriteLine(a.Count());",
                 "3\n");
}
