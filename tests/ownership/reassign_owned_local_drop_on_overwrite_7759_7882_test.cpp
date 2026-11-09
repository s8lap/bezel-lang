// Ownership — the drop-on-overwrite of a REASSIGNED owned LOCAL disposes the
// displaced value (§28.7), for the `!` spelling of a fresh owned class
// producer (issue #7759) and for an owned Task-family local (issue #7882).
//
// Spec, v0.355:
//   sec 28.7: "Assignment to an owned field or owned local disposes the
//   displaced value ... The right-hand side is evaluated first; then, if the
//   previous value is live (non-null - and, for a binding, not moved-from),
//   it is disposed; then the new value is stored. The disposal is null-guarded
//   and dynamically dispatched, exactly the machinery scope-end disposal
//   already uses."
//   sec 17.8: "`x!` - the result is non-nullable, keeps `x`'s OWN
//   classification"; and the producers table: "a fresh producer's `!` is
//   owned." So `h.MkW()!` is an OWNED fresh producer and the binding it
//   initialises is an owned local - exactly as the bare `h.MkW()` is.
//   sec 30.5 / sec 28.7: a Task-family local is owned by the same rule; its
//   displaced frame is disposed through the task protocol, not the class
//   vtable.
//
// THE DEFECTS. Both are drop-on-overwrite gaps, i.e. the §28.7 disposal the
// ownership pass already PROMISES (it classifies the local as Owned and its
// `localDropOnOverwriteCovered` verdict says the overwrite is covered) was
// never emitted by codegen:
//
//   #7759: codegen's class-local drop-on-overwrite classifies the local by
//   its INITIALIZER FORM, and a postfix `!` is invisible to every arm of
//   that classifier - so `var w := h.MkW()!;` was not an owned class local
//   and the displaced value was never disposed. MEASURED on the base
//   (r30-c1): `ds=100` of 200 and 1600 B leaked in 100 allocations over 100
//   iterations, where the byte-identical BARE spelling is clean (`ds=200`).
//
//   #7882: a Task-family local is excluded from the class-local path on
//   purpose (it is freed by `bz_task_free`, not by vtable dispatch), and no
//   drop-on-overwrite existed for it at all - so `var t := W(); t := W();`
//   leaked the displaced frame. MEASURED on the base: LSan reports
//   19104 B in 103 allocations over 100 iterations.
//
// The `!` spelling must MATCH the bare spelling - it must not be a
// special-cased fix of its own - and the negative controls (the bare
// spelling, the single binding, the moved-from binding) must be unchanged.
//
// Every runtime row runs 50x under generated-code ASan/LSan (runAotChecked)
// and pins exact stdout; a leak makes `r.ran` false.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

namespace {

// stdout (trailing newline stripped) of a clean, ASan/LSan-checked run, or a
// marker naming the failure (compile error, non-zero exit incl. a leak
// report).
std::string out(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    std::string s = r.stdoutText;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// `W` counts its own destructions in the static `W.ds`; `H.MkW()` mints a
// fresh `owned W?`.
const char* kPrelude = R"BZL(
module M;
public class W
  public static ds: int := 0;
  public v: int;
  public constructor(v: int) this.v := v; end constructor;
  destructor W.ds := W.ds + 1; end destructor;
end W;
public class H
  public constructor() end constructor;
  public method MkW(): owned W? return new W(3); end MkW;
end H;
public class Sink
  public static method EatW(owned w: W?) end EatW;
end Sink;
)BZL";

std::string classProg(const std::string& body) {
    return std::string(kPrelude) +
           "public class C\n"
           "  public static method Main(): int\n"
           "    var sum := 0; var i := 0;\n"
           "    while i < 50 do\n"
           "      using var h := new H();\n" +
           body +
           "      i := i + 1;\n"
           "    end;\n"
           "    Console.WriteLine(\"sum=\" + sum.ToString() + \" ds=\" + "
           "W.ds.ToString());\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

// An owned Task local: `Program.W()` mints the frame, `Program.Take` consumes
// it under the `move` marker (the §30.5 Pattern-A consume position). `W` is
// SILENT on purpose: the row pins the program's exact stdout, and a frame
// that prints would put 50 lines in front of the one line that matters.
std::string taskProg(const std::string& body) {
    return "module M;\n"
           "public class Program\n"
           "  public static async method W(): Task end W;\n"
           "  public static async method Take(owned x: Task): Task "
           "using var x2 := move x; await x2; end Take;\n"
           "  public static async method Main(): Task\n"
           "    for var i in 0..<50 do\n" +
           body +
           "    end;\n"
           "    Console.WriteLine(\"END\");\n"
           "  end Main;\n"
           "end Program;\n";
}

}  // namespace

// --- #7759: the `!` spelling of a fresh owned class producer -----------------

// The displaced value is disposed: two `W` destructions per iteration.
BEZEL_TEST(ownership_reassign_owned_local_drop_on_overwrite_7759_7882,
            bang_reassign_disposes_the_displaced_value_50x) {
    BEZEL_EXPECT_EQ(
        out(classProg("      var w := h.MkW()!; w := h.MkW()!; "
                      "sum := sum + w.v;\n")),
        "sum=150 ds=100");
}

// The bare spelling is the control: it already disposed both values, and it
// must keep doing so.
BEZEL_TEST(ownership_reassign_owned_local_drop_on_overwrite_7759_7882,
            control_bare_reassign_disposes_the_displaced_value_50x) {
    BEZEL_EXPECT_EQ(
        out(classProg("      var w := h.MkW(); w := h.MkW(); "
                      "sum := sum + w!.v;\n")),
        "sum=150 ds=100");
}

// The `!` spelling WITHOUT a reassignment is the control: one disposal at
// scope end, no double dispose.
BEZEL_TEST(ownership_reassign_owned_local_drop_on_overwrite_7759_7882,
            control_bang_single_binding_disposes_once_50x) {
    BEZEL_EXPECT_EQ(
        out(classProg("      var w := h.MkW()!; sum := sum + w.v;\n")),
        "sum=150 ds=50");
}

// A self-assignment is rejected by the ownership pass (§28.3's state match),
// so it can never reach the drop-on-overwrite.
BEZEL_TEST(ownership_reassign_owned_local_drop_on_overwrite_7759_7882,
            control_self_assignment_is_rejected) {
    auto r = runAotChecked(classProg("      var w := h.MkW()!; w := w;\n"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "cannot self-assign `w`"));
}

// A `!` over a PLACE is a BORROW (§17.8), never a fresh producer, so the
// binding it initialises is a borrow and cannot be transferred with `move`.
// This is the negative control for the `!` arm's gate: if the gate admitted
// a place, `v` would be an OWNED local and this program would compile.
BEZEL_TEST(ownership_reassign_owned_local_drop_on_overwrite_7759_7882,
            control_bang_over_a_place_is_a_borrow) {
    auto r = runAotChecked(classProg(
        "      var w := h.MkW()!; var v := w!; Sink.EatW(move v);\n"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "a borrow cannot be passed where an `owned` value is required"));
}

// --- #7882: an owned Task-family local ---------------------------------------

// The displaced frame is disposed: no leak, and the program still runs.
BEZEL_TEST(ownership_reassign_owned_local_drop_on_overwrite_7759_7882,
            task_reassign_disposes_the_displaced_frame_50x) {
    BEZEL_EXPECT_EQ(
        out(taskProg("      var t := Program.W(); t := Program.W(); "
                     "await Program.Take(move t);\n")),
        "END");
}

// The control: one task local, no reassignment - exactly one disposal.
BEZEL_TEST(ownership_reassign_owned_local_drop_on_overwrite_7759_7882,
            control_task_single_binding_is_clean_50x) {
    BEZEL_EXPECT_EQ(
        out(taskProg("      var t := Program.W(); "
                     "await Program.Take(move t);\n")),
        "END");
}
