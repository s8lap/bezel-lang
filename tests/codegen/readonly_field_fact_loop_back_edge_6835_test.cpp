// fix #6835 (§7.4, v0.340): a `readonly`-field narrowing fact carried into a
// loop does not hold at a use that the loop's back-edge reaches after the
// fact's ROOT was reassigned (or passed `ref`) in the body.
//
//   §7.4 (v0.340), *`readonly` fields narrow*: "The tracked shape is exactly
//   one field off an admitted root — `this._f`, `TypeName._s`, or `v._f`
//   where `v` is a local variable or non-`ref` parameter that is itself
//   narrowing-tracked; a fact rides its root (reassigning `v` drops it)".
//   §7.4 (v0.340), early-exit bullet: "a fact holds at a point only when it
//   holds on every path that reaches it".
//
// WHAT WAS BROKEN. The #6114 back-edge check covered locals' facts only. With
//   var h := new H(new Pt(1, 2)); if h.F <> null then
//     while i < 2 do USE(h.F); h := new H(null); i += 1; end; end;
// the second iteration read the payload of a null box: wrong output for a
// record field (`h.F.X` printed 0), SIGSEGV for a class field. #6758 made the
// same hole reachable through a user-defined operator.
//
// Reject rows assert the new diagnostic, reported at the use. Accept rows are
// loops that must keep compiling — no reassignment, a re-test inside the loop,
// a reassignment only on a `break` path, a `while`-condition test, an
// ambient (`this`) root — and assert exact stdout under the checked oracle.

#include <string>

#include "support/aot_support.hpp"   // runAot, runAotChecked
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kTypes =
    "public record Pt(X: int, Y: int)\n"
    "  public static pure method operator +(a: Pt, b: Pt): Pt => new Pt(a.X + b.X, a.Y + b.Y);\n"
    "end Pt;\n"
    "public class Cc\n"
    "  public V: int;\n"
    "  public constructor(v: int) this.V := v; end constructor;\n"
    "  public static method operator +(a: Cc, b: Cc): int => a.V + b.V;\n"
    "end Cc;\n"
    "public class H\n"
    "  public readonly F: Pt?;\n"
    "  public readonly N: int?;\n"
    "  public readonly G: Cc?;\n"
    "  public constructor(f: Pt?, n: int?, g: Cc?) this.F := f; this.N := n; this.G := g; end constructor;\n"
    "%HMEMBERS%"
    "end H;\n"
    "public class K\n"
    "  public static method NulP(): Pt? return null; end NulP;\n"
    "  public static method NulN(): int? return null; end NulN;\n"
    "  public static method NulC(): owned Cc? return null; end NulC;\n"
    "  public static method Swap(ref h: H): bool h := new H(K.NulP(), K.NulN(), K.NulC()); return true; end Swap;\n"
    "%KMEMBERS%"
    "end K;\n";

std::string replaceOnce(std::string s, const std::string& from,
                        const std::string& to) {
    s.replace(s.find(from), from.size(), to);
    return s;
}

// `body` runs in Main with `h` (all three fields non-null) and `i := 0`. The
// `Cc` objects a body stores into `H.G` are locals declared before `h`, so
// they outlive every `H` that borrows them.
// `hMembers` / `kMembers` are spliced into `H` / `K`.
std::string program(const std::string& body, const std::string& hMembers = "",
                    const std::string& kMembers = "") {
    std::string types = replaceOnce(replaceOnce(kTypes, "%HMEMBERS%", hMembers),
                                    "%KMEMBERS%", kMembers);
    return std::string("module M;\n") + types +
           "public class P\n"
           "  public static method Main(): int\n"
           "    var c3 := new Cc(3); var c8 := new Cc(8);\n"
           "    var h := new H(new Pt(1, 2), 7, c3); var i := 0;\n" +
           body +
           "    return 0;\n"
           "  end Main;\n"
           "end P;\n";
}

// The loop-guarded form every reject row shares.
std::string looped(const std::string& test, const std::string& loopBody) {
    return program("    if " + test + " then\n      while i < 2 do\n        " +
                   loopBody + "\n      end;\n    end;\n");
}

std::string runOut(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) {
        std::string s = "DID NOT RUN:";
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.severity == bezel::Severity::Error) s += " | " + d.message;
        return s;
    }
    return r.stdoutText + "exit=" + std::to_string(r.exitCode);
}

// Rejected AND by the #6835 back-edge diagnostic naming `path`.
bool rejectedAtBackEdge(const std::string& src, const std::string& path) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        const auto& m = d.message;
        if (m.find("`" + path + "` is nullable here") != std::string::npos &&
            m.find("readonly") != std::string::npos &&
            m.find("next iteration") != std::string::npos &&
            m.find("7.4") != std::string::npos)
            return true;
    }
    return false;
}

const std::string kNull = "h := new H(K.NulP(), K.NulN(), K.NulC());";

}  // namespace

// ---- the issue's repro (w08_root_in_loop_{b,m}.bzl), verbatim ---------------

BEZEL_TEST(codegen_readonly_field_loop_back_edge_6835, issue_repro_builtin_call_arg) {
    BEZEL_EXPECT_TRUE(rejectedAtBackEdge(
        "module M;\n"
        "public class H public readonly F: string?; public constructor(f: string?) F := f; end constructor; end H;\n"
        "public class K public static method Take(x: string): int return x.Length; end Take;\n"
        "  public static method Nul(): string? return null; end Nul;\n"
        "  public static method Noop() end Noop;\n"
        "end K;\n"
        "public class Program\n"
        "  public static method Show(s: string?, ni: int?, c: bool, hp: H): int\n"
        "    var h := new H(s); var i := 0; if h.F <> null then while i < 2 do Console.WriteLine(int.Parse(h.F)); h := new H(K.Nul()); i += 1; end; end;\n"
        "    return 0;\n"
        "  end Show;\n"
        "  public static method Main(): int\n"
        "    var hh := new H(\"5\"); return Show(\"5\", 16, true, hh);\n"
        "  end Main;\n"
        "end Program;\n", "h.F"));
}

BEZEL_TEST(codegen_readonly_field_loop_back_edge_6835, issue_repro_member_access) {
    BEZEL_EXPECT_TRUE(rejectedAtBackEdge(
        "module M;\n"
        "public class H public readonly F: string?; public constructor(f: string?) F := f; end constructor; end H;\n"
        "public class K public static method Take(x: string): int return x.Length; end Take;\n"
        "  public static method Nul(): string? return null; end Nul;\n"
        "  public static method Noop() end Noop;\n"
        "end K;\n"
        "public class Program\n"
        "  public static method Show(s: string?, ni: int?, c: bool, hp: H): int\n"
        "    var h := new H(s); var i := 0; if h.F <> null then while i < 2 do Console.WriteLine(h.F.Length); h := new H(K.Nul()); i += 1; end; end;\n"
        "    return 0;\n"
        "  end Show;\n"
        "  public static method Main(): int\n"
        "    var hh := new H(\"5\"); return Show(\"5\", 16, true, hh);\n"
        "  end Main;\n"
        "end Program;\n", "h.F"));
}

// ---- reject rows: the root is invalidated before the back-edge -------------

// The #6758 reviewer's a01: before, printed "11\n10" (a null box read as 0).
BEZEL_TEST(codegen_readonly_field_loop_back_edge_6835, user_operator_record_field) {
    BEZEL_EXPECT_TRUE(rejectedAtBackEdge(looped("h.F <> null",
        "var r := h.F + new Pt(10, 10); Console.WriteLine(r.X); " + kNull + " i += 1;"),
        "h.F"));
}

// The #6758 reviewer's a02: before, SIGSEGV on the second iteration.
BEZEL_TEST(codegen_readonly_field_loop_back_edge_6835, user_operator_class_field) {
    BEZEL_EXPECT_TRUE(rejectedAtBackEdge(looped("h.G <> null",
        "Console.WriteLine(h.G + new Cc(10)); " + kNull + " i += 1;"), "h.G"));
}

// The base-existing member-access controls (a03 SIGSEGV, a31 printed 0).
BEZEL_TEST(codegen_readonly_field_loop_back_edge_6835, member_access_class_field) {
    BEZEL_EXPECT_TRUE(rejectedAtBackEdge(looped("h.G <> null",
        "Console.WriteLine(h.G.V); " + kNull + " i += 1;"), "h.G"));
}

BEZEL_TEST(codegen_readonly_field_loop_back_edge_6835, member_access_record_field) {
    BEZEL_EXPECT_TRUE(rejectedAtBackEdge(looped("h.F <> null",
        "Console.WriteLine(h.F.X + 10); " + kNull + " i += 1;"), "h.F"));
}

BEZEL_TEST(codegen_readonly_field_loop_back_edge_6835, builtin_operator_int_field) {
    BEZEL_EXPECT_TRUE(rejectedAtBackEdge(looped("h.N <> null",
        "Console.WriteLine(h.N + 1); " + kNull + " i += 1;"), "h.N"));
}

// "reassigning `v` drops it" — whatever the new value is.
BEZEL_TEST(codegen_readonly_field_loop_back_edge_6835, non_null_reassignment_still_drops) {
    BEZEL_EXPECT_TRUE(rejectedAtBackEdge(looped("h.F <> null",
        "Console.WriteLine(h.F.X); h := new H(new Pt(5, 5), 9, c8); i += 1;"),
        "h.F"));
}

// A `ref` pass of the root (a parameter: an owned local may not be passed
// `ref` at all). Before: printed "1\n0" off a null box.
BEZEL_TEST(codegen_readonly_field_loop_back_edge_6835, ref_pass_of_root) {
    BEZEL_EXPECT_TRUE(rejectedAtBackEdge(program(
        "    Console.WriteLine(K.Run(h));\n", "",
        "  public static method Run(h: H): int\n"
        "    var i := 0;\n"
        "    if h.F <> null then\n"
        "      while i < 2 do Console.WriteLine(h.F.X); if K.Swap(ref h) then i += 1; end; end;\n"
        "    end;\n"
        "    return 0;\n"
        "  end Run;\n"), "h.F"));
}

BEZEL_TEST(codegen_readonly_field_loop_back_edge_6835, continue_edge) {
    BEZEL_EXPECT_TRUE(rejectedAtBackEdge(looped("h.F <> null",
        "i += 1; Console.WriteLine(h.F.X); if i = 1 then " + kNull +
        " continue; end;"), "h.F"));
}

BEZEL_TEST(codegen_readonly_field_loop_back_edge_6835, labeled_continue_from_inner_loop) {
    BEZEL_EXPECT_TRUE(rejectedAtBackEdge(program(
        "    if h.F <> null then\n"
        "      outer: while i < 2 do\n"
        "        Console.WriteLine(h.F.X); i += 1;\n"
        "        var j := 0;\n"
        "        while j < 1 do " + kNull + " j += 1; continue outer; end;\n"
        "      end;\n"
        "    end;\n"), "h.F"));
}

BEZEL_TEST(codegen_readonly_field_loop_back_edge_6835, reassignment_inside_try) {
    BEZEL_EXPECT_TRUE(rejectedAtBackEdge(looped("h.F <> null",
        "Console.WriteLine(h.F.X); i += 1; try " + kNull +
        " catch var e: Exception => Console.WriteLine(\"x\"), end;"), "h.F"));
}

BEZEL_TEST(codegen_readonly_field_loop_back_edge_6835, reassignment_in_case_arm) {
    BEZEL_EXPECT_TRUE(rejectedAtBackEdge(looped("h.F <> null",
        "Console.WriteLine(h.F.X); case i of 0 => do " + kNull +
        " end, _ => i += 0, end; i += 1;"), "h.F"));
}

BEZEL_TEST(codegen_readonly_field_loop_back_edge_6835, for_in_loop) {
    BEZEL_EXPECT_TRUE(rejectedAtBackEdge(program(
        "    if h.F <> null then\n"
        "      for var k in 0..2 do var b := h.F + new Pt(10, 10); Console.WriteLine(b.X); " +
        kNull + " end;\n"
        "    end;\n"), "h.F"));
}

// A parameter root.
BEZEL_TEST(codegen_readonly_field_loop_back_edge_6835, parameter_root) {
    BEZEL_EXPECT_TRUE(rejectedAtBackEdge(program(
        "    Console.WriteLine(K.Run(h));\n", "",
        "  public static method Run(h: H): int\n"
        "    var i := 0;\n"
        "    if h.F <> null then\n"
        "      while i < 2 do var b := h.F + new Pt(10, 10); Console.WriteLine(b.X); " +
        kNull + " i += 1; end;\n"
        "    end;\n"
        "    return 0;\n"
        "  end Run;\n"), "h.F"));
}

// ---- accept rows: the fact holds at every back-edge -------------------------

BEZEL_TEST(codegen_readonly_field_loop_back_edge_6835, straight_line_no_loop) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    if h.F <> null then Console.WriteLine(h.F.X); Console.WriteLine((h.F + new Pt(1, 1)).Y); end;\n")),
        std::string("1\n3\nexit=0"));
}

BEZEL_TEST(codegen_readonly_field_loop_back_edge_6835, loop_without_reassignment) {
    BEZEL_EXPECT_EQ(runOut(looped("h.F <> null and h.G <> null and h.N <> null",
        "Console.WriteLine((h.F + new Pt(10, 10)).X + h.G.V + (h.G + new Cc(1)) + h.N); i += 1;")),
        std::string("25\n25\nexit=0"));
}

BEZEL_TEST(codegen_readonly_field_loop_back_edge_6835, retest_after_reassignment) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    while i < 3 do\n"
        "      if h.F <> null then Console.WriteLine((h.F + new Pt(10, 0)).X); else Console.WriteLine(-1); end;\n"
        "      if i = 0 then h := new H(new Pt(5, 5), 9, c8); else " + kNull + " end;\n"
        "      i += 1;\n"
        "    end;\n")),
        std::string("11\n15\n-1\nexit=0"));
}

BEZEL_TEST(codegen_readonly_field_loop_back_edge_6835, early_exit_guard_at_top) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    while i < 3 do\n"
        "      if h.G = null then break; end;\n"
        "      Console.WriteLine(h.G + new Cc(10));\n"
        "      if i = 0 then h := new H(new Pt(5, 5), 9, c8); else " + kNull + " end;\n"
        "      i += 1;\n"
        "    end;\n"
        "    Console.WriteLine(i);\n")),
        std::string("13\n18\n2\nexit=0"));
}

// A `break` edge is not a back-edge: the reassignment never reaches the use.
BEZEL_TEST(codegen_readonly_field_loop_back_edge_6835, reassignment_only_on_break_path) {
    BEZEL_EXPECT_EQ(runOut(looped("h.F <> null",
        "Console.WriteLine(h.F.X + i); if i = 1 then " + kNull + " break; end; i += 1;")),
        std::string("1\n2\nexit=0"));
}

// A `while` condition re-tests on every iteration (fix #5331).
BEZEL_TEST(codegen_readonly_field_loop_back_edge_6835, while_condition_retests) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    while h.F <> null do var b := h.F + new Pt(10, 10); Console.WriteLine(b.X); " +
        kNull + " end;\n")),
        std::string("11\nexit=0"));
}

// An ambient (`this`) root is never reassigned: reassigning an unrelated local
// in the loop leaves `this.F`'s fact in place.
BEZEL_TEST(codegen_readonly_field_loop_back_edge_6835, this_root_survives) {
    BEZEL_EXPECT_EQ(runOut(program(
        "    Console.WriteLine(h.Run());\n",
        "  public method Run(): int\n"
        "    var i := 0; var h := new H(K.NulP(), K.NulN(), K.NulC());\n"
        "    if this.F <> null then\n"
        "      while i < 2 do Console.WriteLine((this.F + F).X + i); " + kNull + " i += 1; end;\n"
        "    end;\n"
        "    return i;\n"
        "  end Run;\n")),
        std::string("2\n3\n2\nexit=0"));
}
