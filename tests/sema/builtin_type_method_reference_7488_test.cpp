// Spec-derived regression test for issues #7488 and #7489.
//
// SPEC §27.6 (v0.355, docs/Bezel_Language_Spec.md:6382-6432), *Method
// references*, verbatim:
//
//   "For an extension method, the reference is formed in either of the two
//    spellings a call admits. **Type-qualified**, the reference uses the
//    extended type's name as qualifier, at the type arguments that bind a
//    generic block's parameters (§26.4); it captures no receiver, and the
//    resulting function has the receiver as its first parameter.
//    **Receiver-qualified** — `v.Scaled` over `extension Vec`, the member
//    resolved on the value exactly as the call `v.Scaled()` is (§26.5) — the
//    reference is a bound reference: the receiver is captured at formation
//    … and the resulting function has the method's own parameters only
//    (`var scaled: () -> int := v.Scaled;`)."
//
// and §27.6's own type-qualified example:
//
//   var counter: (IEnumerable<int>) -> int := IEnumerable<int>.Count;
//
// SPEC §26.5 rule 2 (v0.355), *Resolution*, verbatim (docs/Bezel_Language_Spec.md:6115):
//
//   "The type-qualified references §27.6 forms over an instance property or
//    an extension method (`string.Length`, `IEnumerable<int>.Count`) are
//    formations, not such accesses, and are unaffected."
//
// STDLIB §4.4 (v0.355) declares `Count` and `Capacity` on `Array<T>` — and NO
// `Length`; §4.11 declares `Count` (via `IReadOnlyCollection<T>`) on
// `Stack<T>` / `Queue<T>` and their Owning twins — and no `Length` either. So
// §26.5 rule 2 REQUIRES the extension blocks to be consulted for the name
// `Length` on those receivers, and the user's extension is the selected
// candidate.
//
// THE DEFECTS.
//
//   #7488 — a FALSE REJECT, and an ASYMMETRY: the CALL `a.Length()` is
//   admitted (and prints 42), while the BARE REFERENCE `a.Length` is refused
//   with "no member `Length` on `Array<T>`: use `Count` (stdlib §4.4)". §27.6
//   says the reference resolves the member "exactly as the call" does, so the
//   two spellings must agree — the reference is a legal program.
//
//   #7489 — a SILENT DEGRADATION (the #3079 family): the TYPE-QUALIFIED
//   reference `Array<int>.Length` COMPILED with the #2434 warning
//   ("lowerLocalDecl's initializer expression failed to lower to a value for
//   this local's slot (degraded to a zero-initialized value)") and left the
//   slot zero-initialised. MEASURED on the fix's base: the program SEGFAULTS
//   at the call, so the degradation is a null-call crash, not merely a
//   missing print. §27.6 forms the type-qualified reference over an
//   extension method, so the slot must hold the reference and calling it must
//   run the extension.
//
// THE ROWS. The value rows assert the program's exact STDOUT (the oracle is
// the RENDERED VALUE, never the exit code). The rejection rows assert the
// SPECIFIC diagnostic at a real source location. The CONTROL rows are
// load-bearing: with NO extension the reference must STILL be refused (the
// head declares no `Length`), and the sibling `Count` must still be the
// member the receiver answers on.
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The `Array<T>` refusal, with the §-section the diagnostic carries.
const char kArrayLength[] =
    "no member `Length` on `Array<T>`: use `Count` (stdlib \xC2\xA7""4.4)";

// The `Stack<T>` refusal (stdlib §4.11).
const char kStackLength[] =
    "no member `Length` on `Stack<T>`: use `Count` (stdlib \xC2\xA7""4.11)";

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

// The user extension of the two DEFECT rows: a `Length` member over
// `Array<int>`, which the stdlib's `Array<T>` does NOT declare (stdlib §4.4).
const char kArrayExt[] =
    "extension Array<int>\n"
    "  public method Length(): int => 42;\n"
    "end;\n";

}  // namespace

// ── #7488: the RECEIVER-QUALIFIED bare reference ────────────────────────────

// §27.6 (v0.355): the receiver-qualified reference resolves the member "on
// the value exactly as the call `v.Scaled()` is" — the SAME member, so the
// reference is a BOUND reference and calling it runs the EXTENSION.
BEZEL_TEST(sema_builtin_type_method_reference_7488,
           array_length_extension_bare_reference_calls_the_extension) {
    // per §27.6 (v0.355) + §26.5 rule 2 (v0.355) + stdlib §4.4 (v0.355)
    expectStdout(kArrayExt,
                 "var f: () -> int := a.Length;\n"
                 "    Console.WriteLine(f());",
                 "42\n");
}

// The CALL spelling — the arm the bare reference must agree with. It was
// already admitted by #7447; this row pins that the fix did not disturb it.
BEZEL_TEST(sema_builtin_type_method_reference_7488,
           array_length_extension_call_still_answers) {
    // per §27.6 (v0.355) + stdlib §4.4 (v0.355)
    expectStdout(kArrayExt,
                 "Console.WriteLine(a.Length());",
                 "42\n");
}

// The same shape on the `Stack<T>` head (stdlib §4.11 declares `Count`, no
// `Length`), so §26.5 rule 2 applies there too.
BEZEL_TEST(sema_builtin_type_method_reference_7488,
           stack_length_extension_bare_reference_calls_the_extension) {
    // per §27.6 (v0.355) + §26.5 rule 2 (v0.355) + stdlib §4.11 (v0.355)
    auto r = runAot("module M;\n"
                    "extension Stack<int>\n"
                    "  public method Length(): int => 42;\n"
                    "end;\n"
                    "class C\n"
                    "  public static method Main(): int\n"
                    "    var s := new Stack<int>();\n"
                    "    s.Push(1);\n"
                    "    var f: () -> int := s.Length;\n"
                    "    Console.WriteLine(f());\n"
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "42\n");
}

// ── #7489: the TYPE-QUALIFIED reference ────────────────────────────────────

// ── CONTROLS ────────────────────────────────────────────────────────────────

// With NO extension the receiver-qualified reference is STILL refused: the
// head declares `Count` and no `Length`, so there is no member to reference.
BEZEL_TEST(sema_builtin_type_method_reference_7488,
           array_length_without_an_extension_is_still_refused) {
    // per stdlib §4.4 (v0.355) + §26.5 rule 2 (v0.355)
    expectRejected("",
                   "var f: () -> int := a.Length;\n"
                   "    Console.WriteLine(f());",
                   kArrayLength);
}

// And the `Stack<T>` twin (stdlib §4.11).
BEZEL_TEST(sema_builtin_type_method_reference_7488,
           stack_length_without_an_extension_is_still_refused) {
    // per stdlib §4.11 (v0.355) + §26.5 rule 2 (v0.355)
    auto r = runAot("module M;\n"
                    "class C\n"
                    "  public static method Main(): int\n"
                    "    var s := new Stack<int>();\n"
                    "    s.Push(1);\n"
                    "    var f: () -> int := s.Length;\n"
                    "    Console.WriteLine(f());\n"
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, kStackLength));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// The sibling member the receiver DOES declare still answers: `Array<T>`
// declares `Count` (stdlib §4.4), and the Count-vs-Length convention is
// why the diagnostic points at it.
BEZEL_TEST(sema_builtin_type_method_reference_7488,
           array_count_still_answers) {
    // per stdlib §4.4 (v0.355)
    expectStdout("",
                 "Console.WriteLine(a.Count());",
                 "3\n");
}
