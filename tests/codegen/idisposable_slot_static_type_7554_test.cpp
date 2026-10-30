// Spec-derived regression for issue #7554: what an `owned … : IDisposable`
// slot may hold.
//
// THE RULE
// ========
// The owner's ruling on the issue (reading (A)): "the text decides it by
// static type. An `owned F: IDisposable?` slot holds exactly what converts
// implicitly to `IDisposable`. For each such value, the implementation must
// make the slot's `Dispose()` reach the value's real disposal."
//
// spec §8.7 (v0.350): "The compiler generates `IDisposable` on every
// non-static class. Static classes, records, enums, choices, primitives, and
// inline arrays do not implement `IDisposable`; neither do the
// runtime-managed types — `string`, `Type`, boxed values and function
// values"; "`Dispose` is not on `Object`". §5.1's reference conversions reach
// an interface only from a type implementing it or an interface extending it,
// and stdlib §4.12's `IEnumerable<T>` does not extend `IDisposable` (its
// `IEnumerator<T>` does).
//
// THE DEFECTS (both measured on base 26216e9f)
// ============================================
// 1. SEMA ADMITTED THE EXCLUDED POPULATION. `ExprTC::checkAssignable`
//    (src/sema/ExprTypecheck.cpp) had no arm for a built-in `IDisposable`
//    target, so every source reached the catch-all accept: `Take(5)`,
//    `Take(Col.Red)` and `Take(new Rec(1))` into `owned d: IDisposable`
//    aborted the IR verifier ("Call parameter type does not match function
//    signature!"); `Take("abc")`, `Take(new Object())` and `Take(G.Gen())`
//    (an `IEnumerable<int>`) compiled.
// 2. AN ADMITTED CLASS DID NOT REACH ITS DISPOSAL. A
//    `CancellationTokenSource` (one of the ruling's named classes) has no
//    dispatch header: its first word is its state-cell pointer, so the slot's
//    interface `Dispose()` (vtable slot 2, `emitDisposeAndReclaim`) called
//    through a non-vtable and SEGV'd at pc 0. The cell now opens with
//    `bz_cts_vtable` (src/runtime/bezel_runtime_concurrency.cpp), the #6776
//    Mutex shape.
//
// THE ADMITTING TWINS
// ===================
// A class (user, exception, stdlib `Mutex`), a user interface extending
// `IDisposable`, an iterator's `IEnumerator<T>`, `null`, and an `Object`
// carrier flow-narrowed by `is IDisposable` (§12.7) must stay admitted.

#include <string>
#include <string_view>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

constexpr const char* kNotConvertible =
    "which does not implicitly convert to `IDisposable` (§5.1)";

// The declarations every row shares; `main` is the body of `Main`.
std::string prog(const std::string& main, const std::string& extraDecls = "") {
    return
        "module M;\n"
        "import System.Threading;\n"
        "public record Rec(x: int);\n"
        "public enum Col Red, Green; end Col;\n"
        "public interface IPlain\n"
        "  method Foo(): int;\n"
        "end IPlain;\n"
        "public interface IMy: IDisposable\n"
        "  method Foo(): int;\n"
        "end IMy;\n"
        "public class Res: IPlain, IMy\n"
        "  public constructor() end constructor;\n"
        "  public method Foo(): int return 1; end Foo;\n"
        "  destructor\n"
        "    Console.WriteLine(\"RD\");\n"
        "  end destructor;\n"
        "end Res;\n"
        "public class H\n"
        "  public owned F: IDisposable? := null;\n"
        "  public constructor() end constructor;\n"
        "end H;\n"
        "public class G\n"
        "  public static method Gen(): IEnumerable<int>\n"
        "    yield return 1;\n"
        "  end Gen;\n"
        "  public static method Take(owned d: IDisposable)\n"
        "    d.Dispose();\n"
        "  end Take;\n"
        "  public static method AsRet(): IDisposable?\n"
        "    return null;\n"
        "  end AsRet;\n"
        "end G;\n" +
        extraDecls +
        "public class C\n"
        "  public static method Main(): int\n" +
        main +
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

bool hasError(const JitResult& r, std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// The row must be rejected by THIS rule (both the shared needle and the
// population's own reason), not by an IR-verifier abort or another check.
void expectRejected(const std::string& program, std::string_view reason) {
    JitResult r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(hasError(r, reason));
}

void expectAdmitted(const std::string& program, const std::string& out) {
    JitResult r = runAot(program);
    BEZEL_EXPECT_FALSE(hasError(r, kNotConvertible));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

bool skipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("SKIP") == std::string::npos) continue;
        if (d.message.find("sanitizer fired") != std::string::npos) return false;
        return true;
    }
    return false;
}

void expectCleanUnderAsan(const std::string& program, const std::string& out) {
    JitResult r = runAotChecked(program);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// ---- §8.7: the excluded population, at an owned IDisposable parameter -----

BEZEL_TEST(idisposable_slot_static_type_7554, spec_8_7_record_argument_rejected) {
    expectRejected(prog("    G.Take(new Rec(1));\n"),
                   "a record does not implement `IDisposable` (§8.7)");
}

BEZEL_TEST(idisposable_slot_static_type_7554, spec_8_7_enum_argument_rejected) {
    expectRejected(prog("    G.Take(Col.Red);\n"),
                   "an enum does not implement `IDisposable` (§8.7)");
}

BEZEL_TEST(idisposable_slot_static_type_7554, spec_8_7_primitive_argument_rejected) {
    expectRejected(prog("    G.Take(5);\n"),
                   "a primitive does not implement `IDisposable` (§8.7)");
}

BEZEL_TEST(idisposable_slot_static_type_7554, spec_8_7_string_argument_rejected) {
    expectRejected(prog("    G.Take(\"abc\");\n"),
                   "`string` is a runtime-managed type, not a class");
}

BEZEL_TEST(idisposable_slot_static_type_7554, spec_8_7_object_argument_rejected) {
    expectRejected(prog("    G.Take(new Object());\n"),
                   "`Dispose` is not on `Object` (§8.7)");
}

BEZEL_TEST(idisposable_slot_static_type_7554, stdlib_4_12_ienumerable_argument_rejected) {
    expectRejected(prog("    G.Take(G.Gen());\n"),
                   "the interface neither is nor extends `IDisposable`");
}

BEZEL_TEST(idisposable_slot_static_type_7554, spec_8_7_function_value_argument_rejected) {
    expectRejected(prog("    var f := lambda(x: int) => x + 1;\n"
                        "    G.Take(move f);\n"),
                   "a function value is runtime-managed, not a class");
}

BEZEL_TEST(idisposable_slot_static_type_7554, spec_5_1_plain_interface_argument_rejected) {
    expectRejected(prog("    var p: IPlain := new Res();\n"
                        "    G.Take(move p);\n"),
                   "the interface neither is nor extends `IDisposable`");
}

// ---- the same rule at the other §5.1 conversion sites ---------------------

BEZEL_TEST(idisposable_slot_static_type_7554, spec_8_7_record_into_owned_field_rejected) {
    expectRejected(prog("    var h := new H();\n"
                        "    h.F := new Rec(1);\n"),
                   "a record does not implement `IDisposable` (§8.7)");
}

BEZEL_TEST(idisposable_slot_static_type_7554, spec_8_7_string_local_initializer_rejected) {
    expectRejected(prog("    var d: IDisposable := \"abc\";\n"),
                   "`string` is a runtime-managed type, not a class");
}

BEZEL_TEST(idisposable_slot_static_type_7554, stdlib_4_12_ienumerable_assignment_rejected) {
    expectRejected(prog("    var d: IDisposable? := null;\n"
                        "    d := G.Gen();\n"),
                   "the interface neither is nor extends `IDisposable`");
}

BEZEL_TEST(idisposable_slot_static_type_7554, spec_8_7_object_return_rejected) {
    expectRejected(
        prog("", "public class R2\n"
                 "  public static method Get(): IDisposable\n"
                 "    return new Object();\n"
                 "  end Get;\n"
                 "end R2;\n"),
        "`Dispose` is not on `Object` (§8.7)");
}

BEZEL_TEST(idisposable_slot_static_type_7554, spec_8_7_primitive_into_idisposable_array_rejected) {
    expectRejected(prog("    var a := new Array<IDisposable>();\n"
                        "    a.Add(5);\n"),
                   "a primitive does not implement `IDisposable` (§8.7)");
}

// ---- admitting twins: what §8.7 / §5.1 DO convert -------------------------

BEZEL_TEST(idisposable_slot_static_type_7554, spec_8_7_classes_and_extending_interfaces_admitted) {
    expectAdmitted(prog("    var m: IMy := new Res();\n"
                        "    G.Take(move m);\n"
                        "    G.Take(new Res());\n"
                        "    G.Take(new Exception(\"x\"));\n"
                        "    G.Take(new Mutex());\n"
                        "    var d: IDisposable? := null;\n"
                        "    var e := G.AsRet();\n"
                        "    Console.WriteLine(\"end\");\n"),
                   "RD\nRD\nend\n");
}

BEZEL_TEST(idisposable_slot_static_type_7554, stdlib_4_12_ienumerator_admitted) {
    expectAdmitted(prog("    G.Take(G.Gen().GetEnumerator());\n"
                        "    Console.WriteLine(\"end\");\n"),
                   "end\n");
}

// The STATIC admission only. The runtime answer of `o is IDisposable` is not
// pinned: on base it is false for every class instance at `Object` (a
// separate type-test defect, reported with this fix), so the row does not
// assert what the `if` body did. The program's OUTPUT is still exact: the
// `if` body assigns and prints nothing, and `Res`'s destructor prints "RD"
// once at the scope end whichever way the test goes, so stdout is
// "end\nRD\n".
BEZEL_TEST(idisposable_slot_static_type_7554, spec_12_7_object_narrowed_by_is_admitted) {
    expectAdmitted(prog("    var o: Object := new Res();\n"
                        "    if o is IDisposable then\n"
                        "      var d: IDisposable := o;\n"
                        "    end;\n"
                        "    var od := o as IDisposable;\n"
                        "    Console.WriteLine(\"end\");\n"),
                   "end\nRD\n");
}

// ---- the ruling's implementation obligation: CancellationTokenSource ------
// Looped 100x so a per-iteration leak or a single bad free is not lost.

BEZEL_TEST(idisposable_slot_static_type_7554, spec_8_7_cts_owned_param_dispose_reaches_real_disposal) {
    expectCleanUnderAsan(
        prog("    var i := 0;\n"
             "    while i < 100 do\n"
             "      G.Take(new CancellationTokenSource());\n"
             "      i := i + 1;\n"
             "    end while;\n"
             "    Console.WriteLine(\"end\");\n"),
        "end\n");
}

BEZEL_TEST(idisposable_slot_static_type_7554, spec_8_7_cts_auto_drop_and_using_reach_real_disposal) {
    expectCleanUnderAsan(
        prog("    var i := 0;\n"
             "    while i < 100 do\n"
             "      D.Drop(new CancellationTokenSource());\n"
             "      using var u: IDisposable := new CancellationTokenSource();\n"
             "      var c := new CancellationTokenSource();\n"
             "      c.Cancel();\n"
             "      c.Dispose();\n"
             "      i := i + 1;\n"
             "    end while;\n"
             "    Console.WriteLine(\"end\");\n",
             "public class D\n"
             "  public static method Drop(owned d: IDisposable)\n"
             "  end Drop;\n"
             "end D;\n"),
        "end\n");
}

// stdlib §6.1: the CTS destructor "signals cancellation to any tokens still
// observing", so a token taken before the hand-off observes the slot's
// disposal — the slot's `Dispose()` reached the REAL disposal, not a no-op.
BEZEL_TEST(idisposable_slot_static_type_7554, stdlib_6_1_cts_slot_dispose_signals_its_token) {
    expectCleanUnderAsan(
        prog("    var i := 0;\n"
             "    var n := 0;\n"
             "    while i < 50 do\n"
             "      var s := new CancellationTokenSource();\n"
             "      var t := s.Token;\n"
             "      D.Drop(move s);\n"
             "      if t.IsCancellationRequested then n := n + 1; end;\n"
             "      var s2 := new CancellationTokenSource();\n"
             "      var t2 := s2.Token;\n"
             "      G.Take(move s2);\n"
             "      if t2.IsCancellationRequested then n := n + 1; end;\n"
             "      i := i + 1;\n"
             "    end while;\n"
             "    Console.WriteLine(n);\n",
             "public class D\n"
             "  public static method Drop(owned d: IDisposable)\n"
             "  end Drop;\n"
             "end D;\n"),
        "100\n");
}
