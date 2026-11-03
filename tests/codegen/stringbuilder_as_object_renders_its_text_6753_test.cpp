// Spec-derived regression test for issue #6753 — a `StringBuilder` passed
// where an `Object` is expected did not produce its TEXT: `Console.WriteLine(sb)`
// printed four non-printable bytes where the text should be, and
// `sb.Append(sb2)` printed garbage and aborted with exit status 134.
//
// THE SPEC.
//
//   stdlib §2.1 (v0.350) `Console`: "The `Object` overloads invoke
//   `ToString()`."
//
//   stdlib §3.1 (v0.350) declares the `StringBuilder` overload as
//   `public method Append(value: Object): StringBuilder;` - so the
//   `Append` argument is at an `Object` position too, and the `Object`
//   rule of §2.1 applies to it.
//
//   spec §8.5 (v0.350) `ToString`: `ToString` is invoked implicitly inside
//   string interpolation and at an `Object` position; the default for a
//   class is the type's qualified name, and `StringBuilder` DECLARES its
//   own `ToString` (stdlib §3.1), so the value's own rendering is the
//   StringBuilder's TEXT.
//
// THE DEFECT.
//
// The compiler lowered a `StringBuilder` at an `Object` position by reading
// its first word as a vtable pointer, so the "rendered" value was the
// object's four header bytes - not its text.  The second shape did worse:
// `sb.Append(sb2)` appended those same bytes and the program aborted with
// exit status 134.
//
// THE ORACLE IS POSITIVE.  Every row asserts the TEXT the program prints:
// "q" for the StringBuilder's own text, "q" again for the same value at an
// `Object` position, "z" for the appended StringBuilder's text.  A program
// that renders the header bytes prints something else, so the row fails.
//
// The `Object`-position row and the direct row are the same value, so the
// pair is the oracle that the `Object` position renders the TEXT and not
// the bytes.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ===========================================================================
// THE ISSUE'S FIRST REPRO — the StringBuilder rendered at an `Object`
// position must print its TEXT.
//
// stdlib §2.1: "The `Object` overloads invoke `ToString()`."
// ===========================================================================
BEZEL_TEST(stringbuilder_as_object_renders_its_text_6753,
           a_stringbuilder_passed_as_an_object_renders_its_text) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var sb := new StringBuilder(); "
        "    _ := sb.Append(\"q\"); "
        "    var o: Object := sb; "
        "    Console.WriteLine(o); ")),
        std::string("q"));
}

// ===========================================================================
// THE SAME VALUE AT THE `Console.WriteLine` OVERLOAD, beside its own
// `ToString()`.
//
// The two lines must agree: the `Object` overload and the StringBuilder's
// own rendering are the same text (stdlib §2.1, stdlib §3.1).
// ===========================================================================
BEZEL_TEST(stringbuilder_as_object_renders_its_text_6753,
           the_object_overload_and_the_own_tostring_agree) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var sb := new StringBuilder(); "
        "    _ := sb.Append(\"q\"); "
        "    Console.WriteLine(sb.ToString()); "
        "    Console.WriteLine(sb); ")),
        std::string("q\nq"));
}

// ===========================================================================
// THE ISSUE'S SECOND REPRO — `Append(value: Object)` with a StringBuilder
// argument (stdlib §3.1).
//
// The appended value is the other StringBuilder, so the result is that
// StringBuilder's TEXT - "z" - and the program must not abort.
// ===========================================================================
BEZEL_TEST(stringbuilder_as_object_renders_its_text_6753,
           appending_a_stringbuilder_appends_its_text) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var sb := new StringBuilder(); "
        "    var sb2 := new StringBuilder(); "
        "    _ := sb2.Append(\"z\"); "
        "    _ := sb.Append(sb2); "
        "    Console.WriteLine(sb.ToString()); ")),
        std::string("z"));
}
