// fix #9237 (§35.5 *The owning match*, §28.3 K1.3 / K3.3, §19.2 *Arms*,
// v0.359): in an EXPRESSION-position arm of an owning match, TWO moves of the
// same payload binding in ONE call (`G.Two(move v, move v)`) were ADMITTED.
//
// THE RULE. §35.5: in an owning match "an `owned`-payload binding (`var r`
// above) is an owned local of the arm body's scope, and §28.3's standard rules
// apply to it on every arm-exit edge". §28.3 K3.3 (the consume table): "an
// `owned` argument (§22.3) | `move x` at the call site | **moved-from**" — so
// the FIRST `move v` leaves `v` moved-from. §28.3 K1.3: "**moved-from** — the
// obligation has transferred out; the binding is unreadable until reassigned,
// and discharge machinery skips it" — so the SECOND `move v`, in the same call,
// reads a moved-from binding and must be rejected. §19.2: an expression-
// position arm's body is "any Bezel expression", and §28.3's rules are the
// same at every full-expression position (§28.3 K8).
//
// THE BUG. MEASURED on r30-c1 @ 2b3839abd: the program below compiled CLEAN
// (only the unrelated §22.12 / §28.3 auto-drop warnings) and ran clean under
// ASan — the second `move v` handed the already-nulled slot to the callee, so
// the callee's non-nullable `owned Res` parameter received null. The
// statement-position arm (`Ok(var v) => k := G.Two(move v, move v)`), the
// `do … end` arm and a plain owned local were already rejected with "owned
// value in `v` was already moved out at this point (§28.3)".
//
// THE ROOT CAUSE. The ownership pass keeps NO `states_` entry for a pattern
// binding, so `markMovedByName`'s `if (!st) return;` accepted the consume
// SILENTLY. `seedOwningMatchArmBindings` is what gives an owning match's
// transferred payload binding a real `states_` entry; it was called from the
// CFG's statement-position arm block and from `processMarkedArmConstruct`
// (the MARKED-arm path), but NOT from the ordinary (marker-free) `CaseExpr`
// walk — the expression-position path. With the seed, the EXISTING K3.3 /
// K1.3 machinery does the rejecting; no parallel state machine is invented.
//
// OBSERVABILITY. A reject row's pre-fix behaviour is a CLEAN COMPILE, so the
// oracle is the SPECIFIC diagnostic — never a bare `!r.ran` (runAot synthesizes
// an Error for a runtime crash too). The control row is `runAotChecked`
// (AOT + ASan/LSan) and loops 200 times with an exact destructor count.
//
// per §35.5, §28.3 K1.3/K3.3, §19.2, §28.8, v0.359

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot / runAotChecked — the AOT oracles
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

const char* kDecls =
    "public interface IThing method Tag(): int; end IThing; "
    "public class Res: IThing "
    "  public static N: int := 0; "
    "  public static Made: int := 0; "
    "  public constructor() Res.Made := Res.Made + 1; end constructor; "
    "  destructor Res.N := Res.N + 1; end destructor; "
    "  public method Tag(): int return 1; end Tag; "
    "end Res; ";

// The diagnostic the statement-position arm, the `do … end` arm and a plain
// owned local already draw for the same shape (MEASURED on the base).
const char* kAlreadyMoved =
    "owned value in `v` was already moved out at this point (§28.3)";

// The non-generic twin's helper class: `Two` takes TWO `owned Res` slots.
const char* kNonGenericG =
    "public class G "
    "  public static method Sink(owned x: Res): int return 0; end Sink; "
    "  public static method Peek(x: Res): int return 1; end Peek; "
    "  public static method Two(owned x: Res, owned y: Res): int return 2; end Two; "
    "  public static method Use(owned b: Result<Res, string>): int "
    "    var k := 0; "
    "    k := (case move b of Ok(var v) => G.Two(move v, move v), Error(var e) => 2); "
    "    return k; "
    "  end Use; "
    "end G; ";

// The generic twins: `where` is "" (unconstrained T), " where T: class" or
// " where T: IThing".
std::string genGeneric(const std::string& where) {
    return "public class G "
           "  public static method Sink<T>(owned x: T): int" + where +
           " return 0; end Sink; "
           "  public static method Peek<T>(x: T): int" + where +
           " return 1; end Peek; "
           "  public static method Two<T>(owned x: T, owned y: T): int" + where +
           " return 2; end Two; "
           "  public static method Use<T>(owned b: Result<T, string>): int" + where +
           " var k := 0; "
           "k := (case move b of Ok(var v) => G.Two<T>(move v, move v), Error(var e) => 2); "
           "return k; end Use; "
           "end G; ";
}

// The generic-CLASS-scoped twin: the `owned` slots are the class's own `T`.
const char* kGenericClassG =
    "public class H<T> "
    "  public static method Sink(owned x: T): int return 0; end Sink; "
    "  public static method Peek(x: T): int return 1; end Peek; "
    "  public static method Two(owned x: T, owned y: T): int return 2; end Two; "
    "  public static method Use(owned b: Result<T, string>): int "
    "    var k := 0; "
    "    k := (case move b of Ok(var v) => H<T>.Two(move v, move v), Error(var e) => 2); "
    "    return k; "
    "  end Use; "
    "end H; ";

std::string prog(const std::string& g, const std::string& mainBody) {
    return std::string("module M; ") + kDecls + g +
           "public class Program public static method Main(): int " + mainBody +
           " end Main; end Program;";
}

const char* kLoopUse =
    "    var s := 0; var i := 0; "
    "    while i < 200 do "
    "      s := s + G.Use<Res>(Result<Res, string>.Ok(new Res())); "
    "      s := s + G.Use<Res>(Result<Res, string>.Error(\"e\")); "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine($\"N={Res.N} Made={Res.Made} S={s}\"); "
    "    return 0; ";

const char* kLoopUseNonGeneric =
    "    var s := 0; var i := 0; "
    "    while i < 200 do "
    "      s := s + G.Use(Result<Res, string>.Ok(new Res())); "
    "      s := s + G.Use(Result<Res, string>.Error(\"e\")); "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine($\"N={Res.N} Made={Res.Made} S={s}\"); "
    "    return 0; ";

const char* kLoopUseGenericClass =
    "    var s := 0; var i := 0; "
    "    while i < 200 do "
    "      s := s + H<Res>.Use(Result<Res, string>.Ok(new Res())); "
    "      s := s + H<Res>.Use(Result<Res, string>.Error(\"e\")); "
    "      i := i + 1; "
    "    end while; "
    "    Console.WriteLine($\"N={Res.N} Made={Res.Made} S={s}\"); "
    "    return 0; ";

// The reject oracle: the driver REJECTED the program (never ran it) and the
// SPECIFIC diagnostic is among the reported ones.
bool rejectsWith(const std::string& program, const char* needle) {
    JitResult r = runAot(program);
    if (r.ran) return false;
    if (!bezel::test::failedAtCompileTimeWithRealLocation(r)) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// --- the reject rows: one per payload-type shape ---------------------------

BEZEL_TEST(expression_arm_payload_moved_twice_9237,
           spec_28_3_expression_arm_payload_moved_twice_class_typed_rejected) {
    BEZEL_EXPECT_TRUE(
        rejectsWith(prog(kNonGenericG, kLoopUseNonGeneric), kAlreadyMoved));
}

BEZEL_TEST(expression_arm_payload_moved_twice_9237,
           spec_28_3_expression_arm_payload_moved_twice_unconstrained_rejected) {
    BEZEL_EXPECT_TRUE(
        rejectsWith(prog(genGeneric(""), kLoopUse), kAlreadyMoved));
}

BEZEL_TEST(expression_arm_payload_moved_twice_9237,
           spec_28_3_expression_arm_payload_moved_twice_where_class_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog(genGeneric(" where T: class"), kLoopUse), kAlreadyMoved));
}

BEZEL_TEST(expression_arm_payload_moved_twice_9237,
           spec_28_3_expression_arm_payload_moved_twice_where_interface_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog(genGeneric(" where T: IThing"), kLoopUse), kAlreadyMoved));
}

BEZEL_TEST(expression_arm_payload_moved_twice_9237,
           spec_28_3_expression_arm_payload_moved_twice_generic_class_rejected) {
    BEZEL_EXPECT_TRUE(
        rejectsWith(prog(kGenericClassG, kLoopUseGenericClass), kAlreadyMoved));
}

// --- the boundary control --------------------------------------------------
// ONE `move v` in an expression-position arm is §35.5's warning-free discharge
// ("an owned-consuming call (`RenderOwned(move r)`)") and must stay ACCEPTED,
// disposing the payload exactly once (the 200-iteration ASan/LSan leg is the
// check a corpus case cannot express — a wrong fix that over-rejects or drops
// the payload's disposal shows up here and nowhere else).
//
// RED under a plausible wrong fix: one that seeds the binding but leaves the
// payload's §35.5 arm-exit auto-drop off (the `N=200` / `Made=200` counts
// would disagree), or one that rejects the arm's single `move` outright.
BEZEL_TEST(expression_arm_payload_moved_twice_9237,
           control_spec_35_5_expression_arm_single_move_runs_clean) {
    const std::string g =
        "public class G "
        "  public static method Sink(owned x: Res): int return 0; end Sink; "
        "  public static method Two(owned x: Res, owned y: Res): int return 2; end Two; "
        "  public static method Use(owned b: Result<Res, string>): int "
        "    var k := 0; "
        "    k := (case move b of Ok(var v) => G.Sink(move v), Error(var e) => 2); "
        "    return k; "
        "  end Use; "
        "end G; ";
    JitResult r = runAotChecked(prog(g, kLoopUseNonGeneric));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "N=200 Made=200 S=400\n");
}
