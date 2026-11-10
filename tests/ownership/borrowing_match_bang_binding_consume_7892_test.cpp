// The null-forgiving read `r!` of a borrowing `case` arm binding of MOVE-ONLY
// value type is a consume of a borrow exactly as the bare read `r` is
// (issue #7892; spec §17.8 / §15.3 *Pattern binding* / *Local-to-local
// assignment* / *Parameter passing*, §19.3, §35.5, §28.3 K3.12, v0.357).
//
// THE RULE. §17.8: "Over a named owned binding, `x!` is therefore classified
// by its position exactly as the bare read `x` is" -- the `!` spelling is
// transparent. §15.3 *Pattern binding*: "a `case` binding over a move-only
// scrutinee is a **borrow** of the value; consuming operations through the
// binding ... are rejected". So `var x := r!;` is the same MOVE OF A BORROW as
// `var x := r;`, which #4803 rejects with "cannot move the `case` arm binding".
//
// THE BUG (#7892). The rejection recognised only a bare identifier, so every
// `r!` spelling compiled. MEASURED pre-fix, the issue's own repro
// (`case w of Wrap(var r) => do var x := r!; end`) faulted with an ASan
// heap-use-after-free in `T`: `x`'s scope-end pass disposed the payload and
// `w`'s disposed it again.
//
// OBSERVABILITY. The pre-fix behaviour is a CLEAN COMPILE, so the reject oracle
// is the SPECIFIC diagnostic text (absent from the pre-fix compiler), never a
// bare `!r.ran`.
//
// per §17.8, §15.3, §19.3, §35.5, §28.3 K3.12, v0.357

#include <string>

#include "support/aot_support.hpp"  // runAot / runAotChecked -- strict AOT
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kNeedle = "cannot move the `case` arm binding";

const char* kDecls =
    "public class Widget "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Widget._n := Widget._n + 1; end destructor; "
    "  public method Ping(): int => 7; "
    "end Widget; "
    "public choice Outer W(owned a: Widget, owned b: Widget), X; end Outer; ";

std::string prog(const std::string& extra, const std::string& body) {
    return std::string("module M; ") + kDecls +
           "public class Program " + extra +
           "  public static method Main(): int " + body +
           "  end Main; "
           "end Program;";
}

bool rejectsWith(const std::string& program, const char* needle) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// (1) THE ISSUE'S REPRO, verbatim (h13_match_payload_bang.bzl): a nested
// payload binding `Wrap(var r)` consumed through `r!` at a local binding.
BEZEL_TEST(borrowing_match_bang_binding_consume_7892,
           spec_17_8_issue_repro_payload_binding_bang_local_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        "module M;\n"
        "public class Res\n"
        "  public static Count: int := 0;\n"
        "  public constructor() end constructor;\n"
        "  destructor Res.Count := Res.Count + 1; end destructor;\n"
        "end Res;\n"
        "public choice OC Text(owned r: Res), Empty; end OC;\n"
        "public choice Outer Wrap(inner: OC), None; end Outer;\n"
        "public class Hold\n"
        "  public owned G: OC? := null;\n"
        "  public constructor() end constructor;\n"
        "end Hold;\n"
        "public class Program\n"
        "  public static method T(i: int)\n"
        "    var w := Outer.Wrap(OC.Text(new Res()));\n"
        "    case w of\n"
        "      Wrap(var r) => do var x := r!; end,\n"
        "      None => Res.Count := Res.Count\n"
        "    end case;\n"
        "  end T;\n"
        "  public static method Main(): int\n"
        "    for var i in 0..<100 do\n"
        "      T(i);\n"
        "    end;\n"
        "    Console.WriteLine($\"{Res.Count}\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        kNeedle));
}

// (2) Whole-value `var y` binding, local-binding position (the #4803 repro
// with `y!`).
BEZEL_TEST(borrowing_match_bang_binding_consume_7892,
           spec_17_8_var_binding_bang_local_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog("",
        "    var o := Outer.W(new Widget(), new Widget()); "
        "    case o of "
        "      var y => do var z := y!; end "
        "    end case; "
        "    return 0; "), kNeedle));
}

// (3) Assignment position.
BEZEL_TEST(borrowing_match_bang_binding_consume_7892,
           spec_17_8_var_binding_bang_assignment_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog("",
        "    var o := Outer.W(new Widget(), new Widget()); "
        "    var outer := Outer.X; "
        "    case o of "
        "      var y => do outer := y!; end "
        "    end case; "
        "    return 0; "), kNeedle));
}

// (4) Argument position (§15.3 *Parameter passing*: a move-only formal is a
// by-value move).
BEZEL_TEST(borrowing_match_bang_binding_consume_7892,
           spec_17_8_var_binding_bang_argument_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog(
        "  public static method Take(x: Outer): int return 1; end Take; ",
        "    var o := Outer.W(new Widget(), new Widget()); "
        "    case o of "
        "      var y => do var n := Take(y!); Console.WriteLine($\"{n}\"); end "
        "    end case; "
        "    return 0; "), kNeedle));
}

// (5) Discard position (§17.13).
BEZEL_TEST(borrowing_match_bang_binding_consume_7892,
           spec_17_8_var_binding_bang_discard_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog("",
        "    var o := Outer.W(new Widget(), new Widget()); "
        "    case o of "
        "      var y => do _ := y!; end "
        "    end case; "
        "    return 0; "), kNeedle));
}

// (6) Expression-`case` arm result flowing into a local (the arm value is the
// construct's value).
BEZEL_TEST(borrowing_match_bang_binding_consume_7892,
           spec_17_8_expression_arm_result_bang_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog("",
        "    var o := Outer.W(new Widget(), new Widget()); "
        "    var r := (case o of var y => y!); "
        "    return 0; "), kNeedle));
}

// (7) Doubled `!` layers are peeled too (`y!!` is `y`).
BEZEL_TEST(borrowing_match_bang_binding_consume_7892,
           spec_17_8_var_binding_double_bang_local_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog("",
        "    var o := Outer.W(new Widget(), new Widget()); "
        "    case o of "
        "      var y => do var z := y!!; end "
        "    end case; "
        "    return 0; "), kNeedle));
}

// (7b) A `!` over an arm-selecting conditional of borrow bindings is the
// conditional's own classification (§17.8's table: `x!` keeps `x`'s
// classification), which the bare `(if c then y else y)` already rejects.
// Interpretation recorded in the commit message.
BEZEL_TEST(borrowing_match_bang_binding_consume_7892,
           spec_17_8_conditional_of_bindings_bang_local_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(prog("",
        "    var o := Outer.W(new Widget(), new Widget()); "
        "    var c := true; "
        "    case o of "
        "      var y => do var z := (if c then y else y)!; end "
        "    end case; "
        "    return 0; "), kNeedle));
}

// (8) ADMITTING TWIN -- a read THROUGH the borrow (`a!.Ping()` on a payload of
// the borrowing match) is not a whole-value consume and stays admitted; each
// payload is disposed exactly once, by `o`. Loop-amplified, ASan/LSan checked.
BEZEL_TEST(borrowing_match_bang_binding_consume_7892,
           control_spec_17_8_read_through_bang_payload_admitted_dispose_once) {
    auto r = runAotChecked(prog(
        "  public static method Once(): int "
        "    var o := Outer.W(new Widget(), new Widget()); "
        "    var n := 0; "
        "    case o of "
        "      W(var a, _) => do n := a!.Ping(); end, "
        "      X => () "
        "    end case; "
        "    return n; "
        "  end Once; ",
        "    var s := 0; "
        "    for var i in 0..<100 do s := s + Once(); end; "
        "    Console.WriteLine($\"s={s} disposed={Widget._n}\"); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "s=700 disposed=200\n");
}

// (9) ADMITTING TWIN -- a CLASS-typed borrowing binding: `y!` is a borrow of a
// borrow (K3.13: an unmarked read of a class-typed binding never consumes).
BEZEL_TEST(borrowing_match_bang_binding_consume_7892,
           control_spec_17_8_class_typed_binding_bang_is_a_borrow) {
    auto r = runAotChecked(prog(
        "  public static method Once(): int "
        "    using var w := new Widget(); "
        "    var n := 0; "
        "    case w of "
        "      var y => do var z := y!; n := z.Ping(); end "
        "    end case; "
        "    return n; "
        "  end Once; ",
        "    var s := 0; "
        "    for var i in 0..<100 do s := s + Once(); end; "
        "    Console.WriteLine($\"s={s} disposed={Widget._n}\"); "
        "    return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "s=700 disposed=100\n");
}
