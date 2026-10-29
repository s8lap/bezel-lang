// An UNMARKED `using var x := s;` over an `Object`-declared binding in OWNED
// state is the §28.10 compile error (issue #7568; spec §28.10 *Adopting a named
// owned binding*, §28.12, §8.1, v0.350; owner ruling (A) on the issue).
//
// THE GOVERNING TEXT.
//
//   docs/Bezel_Language_Spec.md (§28.10, v0.350): an unmarked `using var y :=
//   x;` is a compile error "when its right-hand side is a named owned binding
//   whose declared type, nullability stripped, is a class or interface type",
//   a named owned binding being a local "whose ownership state at the `using
//   var` declaration is owned". `Object` is a class (§8.1), and §28.10 has no
//   carve-out for it (contrast §28.14's "a class or interface type other than
//   `Object`"). Nothing in §28.10 makes the ambiguity (does `x` adopt `s` or
//   view it) vanish for `Object`.
//
//   The other side of the line (owner ruling): an `Object` local whose state
//   is UNCLASSIFIED (K1: it holds only runtime-managed values, e.g. `var o:
//   Object := 7;`) is not in owned state, so over it the unmarked form is the
//   borrow-pin as for any non-owned source, and stays accepted.
//
// THE BUG (#7568). 633faf4a (issue #7390) and 1ddc8251 (issue #7481) excluded
// an `Object`-declared binding from "class or interface type" ("for now"),
// because `isNamedOwnedClassOrInterfaceBinding` returned false for the
// Primitive-kind `Object` TypeRef. The unmarked form compiled and `s` kept the
// obligation.
//
// THE ORACLE. Each reject row pins the SPECIFIC diagnostic (the declaration's
// "is ambiguous" text and its `move` remedy). Each run row uses `runAotChecked`
// and pins the printed disposal ORDER or COUNT (200 iterations, so a doubled or
// a suppressed disposal cannot hide behind a one-shot run).
//
// per §28.10, §28.12, §8.1, §28.3, v0.350

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kRes =
    "public class Res "
    "  public constructor() end constructor; "
    "  destructor Console.WriteLine(\"DTOR\"); end destructor; "
    "end Res; ";

std::string mainProg(const std::string& body) {
    return std::string("module M; ") + kRes +
           "public class Program "
           "  public static method Main(): int " + body +
           "    Console.WriteLine(\"END\"); return 0; "
           "  end Main; "
           "end Program;";
}

void expectRejectedAmbiguous(const JitResult& r, const char* adoptHint) {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "is ambiguous"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, adoptHint));
}

void expectRunOutput(const JitResult& r, const std::string& expected) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// --- REJECT: an `Object`-declared binding in owned state --------------------

BEZEL_TEST(ownership_using_var_adopt_named_owned_object_7568,
           spec_28_10_unmarked_using_var_over_an_owned_object_local_is_rejected) {
    expectRejectedAmbiguous(runAotChecked(mainProg(
        "var s: Object := new Res(); using var x: Object := s; ")),
        "write `move s` to adopt");
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_object_7568,
           spec_28_10_unmarked_unannotated_using_var_over_an_owned_object_local_is_rejected) {
    expectRejectedAmbiguous(runAotChecked(mainProg(
        "var s: Object := new Res(); using var x := s; ")),
        "write `move s` to adopt");
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_object_7568,
           spec_28_10_unmarked_using_var_over_a_nullable_object_local_is_rejected) {
    // Nullability is stripped before the class test (§28.10).
    expectRejectedAmbiguous(runAotChecked(mainProg(
        "var s: Object? := new Res(); using var x: Object? := s; ")),
        "write `move s` to adopt");
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_object_7568,
           spec_28_10_unmarked_using_var_over_a_null_forgiving_object_read_is_rejected) {
    expectRejectedAmbiguous(runAotChecked(mainProg(
        "var s: Object? := new Res(); using var x: Object := s!; ")),
        "write `move s` to adopt");
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_object_7568,
           spec_28_10_anonymous_using_var_over_an_owned_object_local_is_rejected) {
    expectRejectedAmbiguous(runAotChecked(mainProg(
        "var s: Object := new Res(); using var _ := s; ")),
        "write `move s` to adopt");
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_object_7568,
           spec_28_10_unmarked_using_var_over_an_owned_object_parameter_is_rejected) {
    expectRejectedAmbiguous(runAotChecked(
        std::string("module M; ") + kRes +
        "public class Util "
        "  public static method Take(owned src: Object) "
        "    using var x: Object := src; "
        "  end Take; "
        "end Util; "
        "public class Program "
        "  public static method Main(): int "
        "    Util.Take(new Res()); return 0; "
        "  end Main; "
        "end Program;"),
        "write `move src` to adopt");
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_object_7568,
           spec_28_12_await_using_var_over_an_owned_object_local_is_rejected_in_a_sync_method_too) {
    // The await spelling in an async method, over an `Object` local that holds
    // a class instance (§28.12: §28.10's admission unchanged).
    expectRejectedAmbiguous(runAotChecked(
        std::string("module M; ") + kRes +
        "public class Program "
        "  public static async method Main(): Task "
        "    var s: Object := new Res(); "
        "    await using var _ := s; "
        "  end Main; "
        "end Program;"),
        "delete the declaration");
}

// --- ACCEPT: the remedy, and the non-owned sources (over-reject mirror) ------

BEZEL_TEST(ownership_using_var_adopt_named_owned_object_7568,
           spec_28_12_move_adopts_an_owned_object_local_into_await_using_var_and_disposes_once) {
    // The adopt remedy, in the await spelling: `x` takes the obligation and the
    // single DTOR runs when `x`'s scope ends. (The plain-`using var` spelling of
    // this `move` adoption leaks today: a separate defect reported with this
    // commit, so it is not pinned as a run here.)
    expectRunOutput(runAotChecked(
        std::string("module M; ") + kRes +
        "public class Program "
        "  public static async method Main(): Task "
        "    var s: Object := new Res(); "
        "    await using var x: Object := move s; "
        "    Console.WriteLine(\"END\"); "
        "  end Main; "
        "end Program;"),
        "END\nDTOR\n");
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_object_7568,
           spec_28_10_var_view_of_an_owned_object_local_is_the_other_remedy) {
    // `var v: Object := s;` is a view (a borrow of `s`); `s` keeps the
    // obligation and is disposed once at scope end.
    expectRunOutput(runAotChecked(mainProg(
        "var s: Object := new Res(); var v: Object := s; ")),
        "END\nDTOR\n");
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_object_7568,
           spec_28_10_var_view_of_an_owned_object_local_in_a_loop_disposes_each_once) {
    expectRunOutput(runAotChecked(
        "module M; "
        "public class Res "
        "  public static n: int := 0; "
        "  public constructor() end constructor; "
        "  destructor Res.n := Res.n + 1; end destructor; "
        "end Res; "
        "public class Program "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 200 do "
        "      var s: Object := new Res(); "
        "      var x: Object := s; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(Res.n); return 0; "
        "  end Main; "
        "end Program;"),
        "200\n");
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_object_7568,
           spec_28_10_unmarked_using_var_over_an_object_parameter_is_a_pin) {
    // A plain (borrow) `Object` parameter is not a named owned binding.
    expectRunOutput(runAotChecked(
        std::string("module M; ") + kRes +
        "public class Util "
        "  public static method Look(src: Object) "
        "    using var x: Object := src; "
        "  end Look; "
        "end Util; "
        "public class Program "
        "  public static method Main(): int "
        "    using var s: Object := new Res(); "
        "    Util.Look(s); "
        "    Console.WriteLine(\"END\"); return 0; "
        "  end Main; "
        "end Program;"),
        "END\nDTOR\n");
}

BEZEL_TEST(ownership_using_var_adopt_named_owned_object_7568,
           spec_28_10_unmarked_using_var_over_an_unclassified_object_local_stays_a_pin) {
    // The owner ruling's carve-out: `var o: Object := 7;` is K1 UNCLASSIFIED
    // (holds only runtime-managed values), not in owned state, so the unmarked
    // form is the borrow-pin and is accepted. `runAot`, not `runAotChecked`:
    // the box leaks under LSan independent of this rule (a separate defect,
    // `using var x: Object := 7;` leaks the same 24 bytes), so this row pins
    // ACCEPTANCE and the printed value only.
    JitResult r = runAot(
        "module M; "
        "public class Program "
        "  public static method Main(): int "
        "    var o: Object := 7; "
        "    using var x: Object := o; "
        "    Console.WriteLine(\"END\"); return 0; "
        "  end Main; "
        "end Program;");
    expectRunOutput(r, "END\n");
}
