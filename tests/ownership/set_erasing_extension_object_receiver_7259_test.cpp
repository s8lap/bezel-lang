// Regression tests for issues #7259 and #8786 — §28.3 closure bullet *Set-erasing
// positions*, clause 4 ("Receivers"), v0.359.
//
//   "A function value as the receiver of a member `Object` declares
//    (`f.ToString()`, `f.GetHashCode()`, an interpolation hole) is not at a
//    set-erasing position; as the receiver of an extension call whose head is
//    `Object` — which §26.5 rule 2 reaches from a function-typed receiver
//    (§3.1) — it is, the twin of §9.7's clause for the receiver of such a
//    call."                                                        (spec §28.3)
//
//   "At every set-erasing position a function value with a non-empty source
//    set is rejected, and a capture-free one is admitted"          (spec §28.3)
//
// and the compiler-enforced invariant of Appendix C (v0.359):
//
//   "No function value with a non-empty source set reaches a set-erasing
//    position: a position declared `Object` (the receiver of an extension call
//    headed `Object`, and the target of an `as` at `Object`, included) ..."
//
// THE ISSUE'S OWN REPRO (issue #7259; the shape the verification lane
// r42-v-c3 measured on b279e8daf — "capture-bearing `f` (captures
// `using var` Res) as receiver of `extension Object` call: accepted, prints 1,
// leaks 64 B"):
//
//   using var payload := new Res();
//   var f: () -> int := lambda() => payload.Use();
//   Console.WriteLine(f.Hold());          // `Hold` is `extension Object`
//
// On the base tree that program COMPILES, prints 1, and leaks the closure
// cell (MEASURED: 64 B direct + 8 B indirect under ASan) — the capture set
// is erased at the receiver.  With the fix it is REJECTED, which is what
// §28.3 clause 4 requires.
//
// The capture-free twin of the same shape is base-GREEN (it must stay
// admitted) and is therefore NOT a gtest row: it is the conformance corpus
// case `tests/conformance/rules/cases/s28_3_0119.bzl` (`expect: accept`), and
// a copy sits in this lane's `corpus-candidates/`.
//
// The `?.` spelling is the SAME call (§7.5): MEASURED on the base tree, both
// `f.Hold()` and `f?.Hold()` reach the extension body (the body printed
// "EXT-RAN"), so both put the receiver at that position.
//
// #8786 — the UNCALLED spelling.  §27.6 (v0.359): "every receiver-side rule of
// the call the formation resolves as is read at the formation — §9.7's
// extension-receiver sentence rejects `v.Ext` over a value that holds borrows
// at an `extension Object` as it rejects `v.Ext()`."  So `var g := f.Hold;`
// over a capture-bearing `f` is rejected exactly as `f.Hold()` is (§28.3
// clause 4).  The capture-free, class-receiver and static-reference twins
// stay admitted: conformance-corpus candidates, not gtest rows.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

namespace {

// The issue's own repro, verbatim (issue #7259; the verification lane's
// probe `/var/tmp/bezel-scratch/r42-v-c3/7259.bzl`).
const char* kIssueRepro =
    "module M; "
    "import System; "
    "class Res "
    "  public constructor() end constructor; "
    "  public method Use(): int return 3; end Use; "
    "end Res; "
    "extension Object "
    "  public method Hold(): int => 1; "
    "end; "
    "public class Program "
    "  public static method Main(): int "
    "    using var payload := new Res(); "
    "    var f: () -> int := lambda() => payload.Use(); "
    "    Console.WriteLine(f.Hold()); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

}  // namespace

// REJECT: a capture-bearing function value as the receiver of an extension
// call whose head is `Object` (§28.3 clause 4, v0.359).  RED on the base
// tree — the program compiled there, printed 1 and leaked the closure cell.
BEZEL_TEST(set_erasing_extension_object_receiver_7259,
           capturing_function_value_as_extension_object_receiver_rejected) {
    auto r = runAotChecked(kIssueRepro);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§28.3"));
}

// REJECT: the same position spelled `?.` (§7.5) — the same call, so the same
// verdict.  MEASURED on the base tree: `f?.Hold()` reaches the extension body
// there too, so the capture set is erased at that receiver as well.
BEZEL_TEST(set_erasing_extension_object_receiver_7259,
           capturing_function_value_as_extension_object_receiver_null_conditional_rejected) {
    auto r = runAotChecked(
        std::string("module M; "
                    "import System; "
                    "class Res "
                    "  public constructor() end constructor; "
                    "  public method Use(): int return 3; end Use; "
                    "end Res; "
                    "extension Object "
                    "  public method Hold(): int => 1; "
                    "end; "
                    "public class Program "
                    "  public static method Main(): int "
                    "    using var payload := new Res(); "
                    "    var f: () -> int := lambda() => payload.Use(); "
                    "    var r := f?.Hold(); "
                    "    return 0; "
                    "  end Main; "
                    "end Program; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§28.3"));
}

// REJECT: the same position reached through §12.7's TYPE NARROWING.  §12.7
// (v0.359) reads "a use of a type-narrowed carrier at a position declared
// `Object` ... (an argument, an assignment or initializer, a `return`, ...,
// the receiver of an extension call whose head is such a type) ... as the use
// of `c as T` there ... and takes that `as`'s verdict" — so a carrier
// narrowed to `Object` and then used as the receiver of an extension call
// headed `Object` is read as `f as Object` there, and §28.3 rejects that `as`
// for a capture-bearing function value.  MEASURED on the base tree: the
// program compiled there.
BEZEL_TEST(set_erasing_extension_object_receiver_7259,
           narrowed_capture_bearing_function_value_as_extension_object_receiver_rejected) {
    auto r = runAotChecked(
        std::string("module M; "
                    "import System; "
                    "class Res "
                    "  public constructor() end constructor; "
                    "  public method Use(): int return 3; end Use; "
                    "end Res; "
                    "extension Object "
                    "  public method Hold(): int => 1; "
                    "end; "
                    "public class Program "
                    "  public static method Main(): int "
                    "    using var payload := new Res(); "
                    "    var f: () -> int := lambda() => payload.Use(); "
                    "    if f is Object then "
                    "      var r := f.Hold(); "
                    "      Console.WriteLine(r); "
                    "    end; "
                    "    return 0; "
                    "  end Main; "
                    "end Program; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§28.3"));
}

// #8786 — REJECT: the uncalled spelling `f.Hold`, a §27.6 method-reference
// FORMATION over a capture-bearing function value at an `extension Object`
// receiver (§27.6 + §28.3 clause 4, v0.359).  RED on the base tree: the call
// form was rejected since #7259 but this formation compiled.
BEZEL_TEST(set_erasing_extension_object_receiver_7259,
           capturing_function_value_extension_object_method_reference_rejected_8786) {
    auto r = runAotChecked(
        std::string("module M; "
                    "import System; "
                    "class Res "
                    "  public constructor() end constructor; "
                    "  public method Use(): int return 3; end Use; "
                    "end Res; "
                    "extension Object "
                    "  public method Hold(): int => 1; "
                    "end; "
                    "public class Program "
                    "  public static method Main(): int "
                    "    using var payload := new Res(); "
                    "    var f: () -> int := lambda() => payload.Use(); "
                    "    var g: () -> int := f.Hold; "
                    "    Console.WriteLine(g()); "
                    "    return 0; "
                    "  end Main; "
                    "end Program; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "may not be the receiver of an extension call whose head is "
           "`Object`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§28.3"));
}

// #8786 — REJECT: the same formation at a CALL ARGUMENT (a formation site,
// §27.6), so the check is not tied to the local-initializer spelling.
BEZEL_TEST(set_erasing_extension_object_receiver_7259,
           capturing_function_value_extension_object_method_reference_argument_rejected_8786) {
    auto r = runAotChecked(
        std::string("module M; "
                    "import System; "
                    "class Res "
                    "  public constructor() end constructor; "
                    "  public method Use(): int return 3; end Use; "
                    "end Res; "
                    "extension Object "
                    "  public method Hold(): int => 1; "
                    "end; "
                    "public class Program "
                    "  static method Take(h: () -> int): int return h(); end Take; "
                    "  public static method Main(): int "
                    "    using var payload := new Res(); "
                    "    var f: () -> int := lambda() => payload.Use(); "
                    "    Console.WriteLine(Take(f.Hold)); "
                    "    return 0; "
                    "  end Main; "
                    "end Program; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "may not be the receiver of an extension call whose head is "
           "`Object`"));
}
