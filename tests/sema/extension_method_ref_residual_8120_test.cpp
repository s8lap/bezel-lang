// Spec-derived regression test for issue #8120.
//
// SPEC §27.6 (v0.355, docs/Bezel_Language_Spec.md:6382-6432), *Method
// references*, verbatim:
//
//   "For an extension method, the reference is formed in either of the two
//    spellings a call admits. **Type-qualified**, the reference uses the
//    extended type's name as qualifier … **Receiver-qualified** — `v.Scaled`
//    over `extension Vec`, the member resolved on the value exactly as the
//    call `v.Scaled()` is (§26.5) — the reference is a bound reference: the
//    receiver is captured at formation … and the resulting function has the
//    method's own parameters only (`var scaled: () -> int := v.Scaled;`)."
//
// SPEC §26.5 rule 2 (v0.355), *Resolution*, verbatim:
//
//   "If the receiver's type declares no instance member of that name, the
//    compiler searches the current module and all imported modules … for
//    extension blocks on `x`'s type, base types, or implemented interfaces …
//    The outcome is the same for every member access on a value, not only a
//    call — a property or field read or write, and a method reference, which
//    resolves its member exactly as the call does (§27.6) …"
//
// STDLIB §4.4 (v0.355) declares `Count` and `Capacity` on `Array<T>` and no
// `Doubled`; §4.11 declares `Count` on `Stack<T>` / `Queue<T>` and their Owning
// twins, and no `Doubled` either. So §26.5 rule 2 REQUIRES the extension blocks
// to be consulted for that name on those receivers, and the user's extension is
// the selected candidate.
//
// THE DEFECT — a FALSE REJECT, and an ASYMMETRY between two spellings of one
// program. MEASURED on this fix's base:
//
//   extension Array<int> public method Doubled(): int => this.Count * 2; end;
//   var a: Array<int> := [1,2,3];
//
//   Console.WriteLine(a.Doubled());
//   -> ADMITTED, prints 6          (the CALL)
//
//   var f: () -> int := a.Doubled;
//   -> REFUSED: "no member `Doubled` on `Array<T>` (stdlib §4.4/§4.5) -- it is
//      not part of this collection's documented property surface (Count,
//      Capacity, and the `.Length` alias), a declared method/LINQ operator
//      (call or bare reference, language spec §27.6), or a universal
//      `ToString`/`Equals`/`GetHashCode`/`GetType` member (language spec
//      §8.1)"
//
// §27.6 requires the bare reference to resolve the member "exactly as the call"
// does. The call is admitted, so the reference MUST be admitted: a legal
// program was refused. #7488 fixed this for the ONE name the stdlib heads
// happen to disagree about (`Length`); every other extension member name was
// still refused on the built-in collection heads.
//
// THE ROWS. The value rows assert the program's exact STDOUT (the oracle is
// the RENDERED VALUE, never the exit code). The rejection rows assert the
// SPECIFIC diagnostic at a real source location. The CONTROL rows are
// load-bearing: with NO extension the reference must STILL be refused (the head
// declares no `Doubled`), the CALL must still answer, and the sibling member
// the receiver DOES declare must still answer.
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The `Array<T>` refusal, with the §-section the diagnostic carries.
const char kArrayDoubled[] =
    "no member `Doubled` on `Array<T>` (stdlib \xC2\xA7""4.4/\xC2\xA7""4.5)";

// The `Stack<T>` refusal (stdlib §4.11).
const char kStackDoubled[] =
    "no member `Doubled` on `Stack<T>` (stdlib \xC2\xA7""4.11)";

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

// The user extension of the DEFECT rows: a `Doubled` member over
// `Array<int>`, which the stdlib's `Array<T>` does NOT declare (stdlib §4.4).
const char kArrayExt[] =
    "extension Array<int>\n"
    "  public method Doubled(): int => this.Count * 2;\n"
    "end;\n";

// A member with a parameter of its own — the reference's function type is
// then `(int) -> int`, not `() -> int` (§27.6: "the resulting function has the
// method's own parameters only").
const char kArrayExtParam[] =
    "extension Array<int>\n"
    "  public method Plus(x: int): int => this.Count + x;\n"
    "end;\n";

}  // namespace

// ── #8120: the RECEIVER-QUALIFIED bare reference over a built-in head ───────

// §27.6 (v0.355): the receiver-qualified reference resolves the member "on
// the value exactly as the call `v.Scaled()` is" — the SAME member, so the
// reference is a BOUND reference and calling it runs the EXTENSION.
BEZEL_TEST(sema_extension_method_ref_residual_8120,
           array_extension_member_bare_reference_calls_the_extension) {
    // per §27.6 (v0.355) + §26.5 rule 2 (v0.355) + stdlib §4.4 (v0.355)
    expectStdout(kArrayExt,
                 "Console.WriteLine(a.Doubled());\n"
                 "    var f: () -> int := a.Doubled;\n"
                 "    Console.WriteLine(f());",
                 "6\n6\n");
}

// The member's OWN parameters are the reference's parameters (§27.6): the
// reference to `Plus(x: int)` is `(int) -> int`, and it runs the extension.
BEZEL_TEST(sema_extension_method_ref_residual_8120,
           array_extension_member_with_a_parameter_bare_reference) {
    // per §27.6 (v0.355) + §26.5 rule 2 (v0.355)
    expectStdout(kArrayExtParam,
                 "var f: (int) -> int := a.Plus;\n"
                 "    Console.WriteLine(f(10));",
                 "13\n");
}

// The CALL spelling — the arm the bare reference must agree with. It was
// already admitted; this row pins that the fix did not disturb it.
BEZEL_TEST(sema_extension_method_ref_residual_8120,
           array_extension_member_call_still_answers) {
    // per §27.6 (v0.355) + stdlib §4.4 (v0.355)
    expectStdout(kArrayExt, "Console.WriteLine(a.Doubled());", "6\n");
}

// The `Stack<T>` head (stdlib §4.11 declares `Count`, no `Doubled`), so
// §26.5 rule 2 applies there too — the twin of the Array row.
BEZEL_TEST(sema_extension_method_ref_residual_8120,
           stack_extension_member_bare_reference_calls_the_extension) {
    // per §27.6 (v0.355) + §26.5 rule 2 (v0.355) + stdlib §4.11 (v0.355)
    auto r = runAot("module M;\n"
                    "extension Stack<int>\n"
                    "  public method Doubled(): int => 42;\n"
                    "end;\n"
                    "class C\n"
                    "  public static method Main(): int\n"
                    "    var s := new Stack<int>();\n"
                    "    s.Push(1);\n"
                    "    var f: () -> int := s.Doubled;\n"
                    "    Console.WriteLine(f());\n"
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// ── CONTROLS ────────────────────────────────────────────────────────────────

// With NO extension the receiver-qualified reference is STILL refused: the
// head declares `Count` and no `Doubled`, so there is no member to reference.
BEZEL_TEST(sema_extension_method_ref_residual_8120,
           array_member_without_an_extension_is_still_refused) {
    // per stdlib §4.4 (v0.355) + §26.5 rule 2 (v0.355)
    expectRejected("",
                   "var f: () -> int := a.Doubled;\n"
                   "    Console.WriteLine(f());",
                   kArrayDoubled);
}

// And the `Stack<T>` twin (stdlib §4.11).
BEZEL_TEST(sema_extension_method_ref_residual_8120,
           stack_member_without_an_extension_is_still_refused) {
    // per stdlib §4.11 (v0.355) + §26.5 rule 2 (v0.355)
    auto r = runAot("module M;\n"
                    "class C\n"
                    "  public static method Main(): int\n"
                    "    var s := new Stack<int>();\n"
                    "    s.Push(1);\n"
                    "    var f: () -> int := s.Doubled;\n"
                    "    Console.WriteLine(f());\n"
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, kStackDoubled));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// The sibling member the receiver DOES declare still answers: `Array<T>`
// declares `Count` (stdlib §4.4), and the Count-vs-Length convention is why
// the diagnostic points at the documented surface. `Count` is a PROPERTY
// (`public pure readonly property Count: int`), so it is read, not called:
// `a.Count()` is refused by #7543 (§26.5 rule 1).
BEZEL_TEST(sema_extension_method_ref_residual_8120,
           array_count_still_answers) {
    // per stdlib §4.4 (v0.355)
    expectStdout("", "Console.WriteLine(a.Count);", "3\n");
}
