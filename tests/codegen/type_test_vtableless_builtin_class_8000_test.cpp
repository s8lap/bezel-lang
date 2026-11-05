// Codegen — spec §12.7 / §19.3 / §8.2, v0.350 — issue #8000.
//
// THE RULE. Spec §12.7 (v0.350), "Type tests: `is`, `is not`, `as`":
//
//   "`is T` tests whether a value is an instance of `T` and produces `bool`."
//
// and §19.3 (v0.350) is the type-pattern rule that a type test must be
// ANSWERABLE at run time. §8.2 (v0.350) fixes every heap object's runtime
// type identity at allocation, and §8.1 (v0.350) is the root-`Object` surface.
//
// WHAT WAS BROKEN. For a carrier whose DECLARED type is `Object`, the runtime
// type tag is the only way to answer an `is T` test — the carrier erases the
// static type. `Emitter::lowerTypeTest`
// (`src/codegen/LlvmBackend_Layout.cpp`) resolves the target's runtime
// descriptor from `boxDescriptorFor` / `descArray_` / `builtinDesc_` /
// `runtimeBuiltinDescriptorFor`, and when none resolves it takes the
// `!targetDesc` bail.
//
// TWO defects lived in that bail:
//
// (a) `runtimeBuiltinDescriptorFor` mapped only `StringBuilder` → `bz_sb_desc`
//     and `Mutex` → `bz_mutex_desc`. The runtime ALSO defines and exports
//     `bz_cts_desc` (fix #7554, §8.7 / §5.1, v0.350: `bz_cts_alloc` stamps
//     `src->vtable = bz_cts_vtable`, `bz_cts_vtable[0] = bz_cts_desc`), so the
//     runtime tag EXISTED and the map simply omitted it. `o is
//     CancellationTokenSource` at an `Object` carrier was therefore DROPPED
//     and read as `false` — a CONFIDENT WRONG ANSWER for a value that IS a
//     CancellationTokenSource.
//
// (c) the bail reported a diagnostic ONLY when `concreteTarget` held, and
//     `concreteTarget` requires a user `declaration` (or a primitive / unit /
//     inline-array / constructed-generic target). A DECLARATION-LESS builtin
//     CLASS target (`Stopwatch`, `FileStream`, `Random`, `TaskScope`, `Lock`,
//     `Semaphore`, `SemaphoreToken`, …) is not concrete, so the test was
//     DROPPED SILENTLY: `lowerIf` branched PAST the body, the controlled block
//     never ran, and NOTHING was reported.
//
// MEASURED on the base (`bd1f8788`, the staging tip carrying #7991's
// narrowing fix), same probes as this file's rows:
//
//   * `var o: Object := Stopwatch.StartNew(); if o is Stopwatch then
//     o.Stop(); Console.WriteLine(o.IsRunning); end;` — COMPILES (exit 0),
//     prints NOTHING, and reports NO diagnostic about the dropped test;
//   * the `is not` spelling PRINTS (the dropped test falls through to the
//     else branch) — so the same program shape answers "yes" and "no"
//     depending only on the spelling. That asymmetry is the soundness hole;
//   * `Console.WriteLine(o is Stopwatch)` does not lower to a value at all:
//     "internal: the argument of this `Console.WriteLine` failed to lower to a
//     value, so the whole statement emits nothing … #3079";
//   * `var o: Object := cts; if o is CancellationTokenSource then …` prints
//     "CTS-NO" — the WRONG answer for a value that IS a CTS.
//
// THE FIX. (a) maps `CancellationTokenSource` → `bz_cts_desc` in
// `runtimeBuiltinDescriptorFor` — a pure omission, the runtime tag was
// already there. (c) makes the `!targetDesc` bail REPORT the drop for a
// declaration-less builtin CLASS target, so the answer is never silent.
//
// PART (b) — narrow #8000 (§12.7 / §19.3 / §8.2, v0.355). The seven
// vtable-less builtin CLASSES that had NO runtime tag (`Stopwatch`,
// `TaskScope`, `FileStream`, `Random`, `Lock`, `Semaphore`,
// `SemaphoreToken`) now carry one: each runtime handle opens with a word-0
// vtable in the class slot layout (the #6776 Mutex / #7554 CTS shape —
// bezel_runtime_refcounting.cpp's BZ_BUILTIN_TAG rows), stamped at
// allocation, and `runtimeBuiltinDescriptorFor` maps each name to its
// descriptor. So `o is Stopwatch` at an `Object` carrier is ANSWERED by the
// ordinary `emitIsA` address walk, the controlled block runs, `as` yields the
// value or null, and `o.ToString()` / an owned-`IDisposable` `Dispose()`
// dispatch through real slots (both faulted on a word-0 DATA pointer before).
//
// WHAT IS STILL LEFT (why this is `narrow`). The remaining declaration-less
// builtin classes with no tag (`Task`, `Subscription`, `CancellationToken`,
// the serializer/deserializer classes) still take the (c) bail: the drop is
// LOUD (the `internal:` Warning), not answered. The `residual_*` row below
// pins that the Warning still fires for one of them.
//
// ORACLE. Every accept row RUNS the program and asserts EXACT stdout; the
// diagnostic row compiles the program with the real `bezelc` binary as a
// SUBPROCESS and asserts the compiler's stderr — because `runAot` fills
// `JitResult::diagnostics` only on a REJECT, so a warning on a program that
// COMPILES is invisible through that channel (the #3138 vacuity trap).
//
// Cite: spec §12.7, §19.3, §8.2, §8.1, §8.7 (v0.350); stdlib §6.1 / §6.10 /
// §7.2 / §8.1.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "support/aot_support.hpp"
#include "support/build_paths.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Runs `program` and returns its stdout, or a marker when it did not run.
// A row that asserts on this marker FAILS, so a rejected or crashed program
// can never be mistaken for a passing one.
std::string stdoutOf(const std::string& program) {
    auto r = runAot(program);
    if (!r.ran) return std::string("<did not run>");
    return r.stdoutText;
}

// The same, under the strict ASan/LSan oracle (runAotChecked returns
// ran=false on any sanitizer report, leaks included).
std::string checkedStdoutOf(const std::string& program) {
    auto r = runAotChecked(program);
    if (!r.ran) return std::string("<did not run>");
    return r.stdoutText;
}

// Compiles `program` with the REAL `bezelc` binary as a SUBPROCESS and
// captures the compiler's stderr. `-c` compiles to an object without linking
// (the same shape every diagnostic lit row uses — see
// tests/lit/type_test_covariant_interface_carrier_warns_5077.bzl), and the
// type-test lowering runs inside it, so the diagnostic this file asserts on is
// produced by the same code path `runAot` exercises.
//
// WHY A SUBPROCESS and not the in-process Driver: the compiler prints its
// diagnostics to `std::cerr`, and `runAot` DISCARDS the captured stderr when
// the compile SUCCEEDS (tests/support/aot_support.hpp: "the captured compiler
// stderr is DISCARDED on CompileOutcome::Ok"). A warning on a program that
// compiles is therefore invisible through `JitResult::diagnostics` — and
// asserting its ABSENCE there would be vacuous (#3138). The in-process JIT
// harness is deprecated for new tests (CLAUDE.md), so the real binary is the
// oracle.
struct CompileOutput {
    bool ran = false;
    int exitCode = -1;
    std::string text;  // the compiler's stderr (stdout is folded into it)
};

CompileOutput compileCapturingStderr(const std::string& program) {
    CompileOutput out;
    const auto& dir = wtScratchDir("wt-8000-diag");
    const std::string stem = unique_stem("bezel_8000_");
    const std::string srcPath = (dir / (stem + ".bzl")).string();
    const std::string objPath = (dir / (stem + ".o")).string();
    {
        std::ofstream f(srcPath);
        f << program;
    }
    const std::string cmd = "'" +
                            std::string(bezel::test::build_paths::kBezelcPath) +
                            "' -c -o '" + objPath + "' '" + srcPath + "' 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[4096];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p))
        out.text.append(buf, n);
    const int status = pclose(p);
    out.exitCode =
        (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    out.ran = true;
    std::remove(srcPath.c_str());
    std::remove(objPath.c_str());
    return out;
}

// The needle the (c) diagnostic must carry. It names the dropped test, the
// class that has no runtime tag, and the issue — so a diagnostic that merely
// says "something went wrong" cannot satisfy it.
const char* kDroppedClassNeedle =
    "internal: this type test was DROPPED and reads as false";

// The issue's own repro, verbatim in shape: an `Object` carrier holding a
// `Stopwatch`, tested with `is`. §8.1 (v0.350) is the `Stopwatch`
// declaration; stdlib §8.1 is `Stopwatch.StartNew()`.
const char* kStopwatchIsProgram = R"bzl(module M8000S;
import System.Diagnostics;
public class Program
  public static method Main(): int
    var o: Object := Stopwatch.StartNew();
    if o is Stopwatch then
      o.Stop();
      Console.WriteLine(o.IsRunning);
    end;
    return 0;
  end Main;
end Program;
)bzl";

// The same shape with the `is not` spelling — the CONTROL. §12.7 (v0.350):
// "for `is not T`, the variable is narrowed to `T` in the else branch".
const char* kStopwatchIsNotProgram = R"bzl(module M8000N;
import System.Diagnostics;
public class Program
  public static method Main(): int
    var o: Object := Stopwatch.StartNew();
    if o is not Stopwatch then
      Console.WriteLine("then");
    else
      Console.WriteLine("else");
    end;
    return 0;
  end Main;
end Program;
)bzl";

// A `CancellationTokenSource` at an `Object` carrier — the class the runtime
// ALREADY tags (fix #7554, §8.7 / §5.1, v0.350), so its `is` test is
// ANSWERABLE and must be answered.
const char* kCtsProgram = R"bzl(module M8000C;
import System.Threading;
public class Program
  public static method Main(): int
    using var cts := new CancellationTokenSource();
    var o: Object := cts;
    if o is CancellationTokenSource then
      Console.WriteLine("in");
    else
      Console.WriteLine("out");
    end;
    return 0;
  end Main;
end Program;
)bzl";

}  // namespace

// ── (b) THE TEST ON A NEWLY TAGGED CLASS IS ANSWERED ───────────────────────
// §12.7 (v0.355): "`is T` tests whether a value is an instance of `T` and
// produces `bool`." `Stopwatch` now carries a runtime type tag, so the test
// is ANSWERED and is no longer a dropped one: the (c) Warning must NOT fire.
// ⚠ FLIPPED by narrow #8000 part (b): this row asserted the Warning PRESENT
// (the drop was diagnosed, not answered). The subprocess compile is the
// non-vacuous oracle for an absence (the #3138 trap); the companion
// `residual_class_drop_is_still_diagnosed` row proves the same needle is
// still produced where a drop remains, so this absence cannot be vacuous.
BEZEL_TEST(codegen_type_test_8000,
           is_test_on_vtableless_builtin_is_answered) {
    const CompileOutput out = compileCapturingStderr(kStopwatchIsProgram);
    BEZEL_EXPECT_TRUE(out.ran);
    BEZEL_EXPECT_EQ(out.exitCode, 0);
    BEZEL_EXPECT_TRUE(out.text.find(kDroppedClassNeedle) == std::string::npos);
    BEZEL_EXPECT_TRUE(out.text.find("#8000") == std::string::npos);
}

// The issue's repro, RUN under the ASan/LSan oracle: the controlled block
// runs, `o.Stop()` stops the watch, and `IsRunning` is `False`.
// ⚠ FLIPPED by narrow #8000 part (b): this row pinned `""` (the dropped test
// read as false and the block never ran).
BEZEL_TEST(codegen_type_test_8000,
           is_test_on_vtableless_builtin_is_not_silently_dropped) {
    BEZEL_EXPECT_EQ(checkedStdoutOf(kStopwatchIsProgram),
                    std::string("False\n"));
}

// ── THE `is not` SPELLING — THE CONTROL ─────────────────────────────────────
// §12.7 (v0.350): "for `is not T`, the variable is narrowed to `T` in the else
// branch". The `else` branch runs (it did on the base too — the dropped test
// falls through to it), so this row must stay GREEN on both arms: it is the
// blast radius of (c), and a control that flipped would be a regression.
BEZEL_TEST(codegen_type_test_8000,
           is_not_test_block_runs_when_false) {
    BEZEL_EXPECT_EQ(stdoutOf(kStopwatchIsNotProgram), std::string("else\n"));
}

// ── (a) THE RUNTIME TAG THAT ALREADY EXISTED IS NOW MAPPED ─────────────────
// §12.7 (v0.350) + §8.2 (v0.350) + §8.7 (v0.350): a
// `CancellationTokenSource` carries the runtime type tag `bz_cts_desc`
// (fix #7554), so `o is CancellationTokenSource` at an `Object` carrier is
// ANSWERABLE — and must be answered. On the base the test was dropped and
// read as `false`: the `else` branch ran and "in" was never printed.
BEZEL_TEST(codegen_type_test_8000, cts_is_test_is_answered) {
    BEZEL_EXPECT_EQ(stdoutOf(kCtsProgram), std::string("in\n"));
}

// §12.7 (v0.350): the `is` test PRODUCES A BOOL. The same carrier, printed
// directly, is `True` — not a dropped statement that emits nothing (the
// base's #3079 symptom).
BEZEL_TEST(codegen_type_test_8000, cts_is_test_produces_a_bool) {
    BEZEL_EXPECT_EQ(
        stdoutOf(R"bzl(module M8000D;
import System.Threading;
public class Program
  public static method Main(): int
    using var cts := new CancellationTokenSource();
    var o: Object := cts;
    Console.WriteLine(o is CancellationTokenSource);
    return 0;
  end Main;
end Program;
)bzl"),
        std::string("True\n"));
}

// §12.7 (v0.350): the `is not` spelling of the SAME test. `o` IS a
// CancellationTokenSource, so `o is not CancellationTokenSource` is `false`
// and the ELSE branch runs. On the base the answer was accidentally the same
// (the dropped test fell through to the else branch) — so this row is a
// CONTROL that must stay green, and it is what proves that mapping the
// descriptor did not flip the `is not` spelling.
BEZEL_TEST(codegen_type_test_8000, cts_is_not_test_is_answered) {
    BEZEL_EXPECT_EQ(
        stdoutOf(R"bzl(module M8000E;
import System.Threading;
public class Program
  public static method Main(): int
    using var cts := new CancellationTokenSource();
    var o: Object := cts;
    if o is not CancellationTokenSource then
      Console.WriteLine("then");
    else
      Console.WriteLine("else");
    end;
    return 0;
  end Main;
end Program;
)bzl"),
        std::string("else\n"));
}

// ── THE CONTROLS: THE BLAST RADIUS ─────────────────────────────────────────
// A USER class at an `Object` carrier still answers its `is` test — the
// descriptor chain walk is unchanged by this fix.
BEZEL_TEST(codegen_type_test_8000, user_class_is_test_is_unchanged) {
    BEZEL_EXPECT_EQ(
        stdoutOf(R"bzl(module M8000U;
public class Dog
  public method Speak(): string
    return "woof";
  end Speak;
end Dog;
public class Program
  public static method Main(): int
    var o: Object := new Dog();
    if o is Dog then
      Console.WriteLine("dog");
    else
      Console.WriteLine("not-dog");
    end;
    return 0;
  end Main;
end Program;
)bzl"),
        std::string("dog\n"));
}

// §12.7 (v0.350): "A null value is an instance of no type". A PLAIN `Object`
// is not a `CancellationTokenSource` — the test answers `false`, and nothing
// crashes (the `emitIsA` walk over a plain object's descriptor chain is a
// miss, not a fault).
BEZEL_TEST(codegen_type_test_8000, is_test_on_a_plain_object_is_false) {
    BEZEL_EXPECT_EQ(
        stdoutOf(R"bzl(module M8000P;
import System.Threading;
public class Program
  public static method Main(): int
    var o: Object := new Object();
    if o is CancellationTokenSource then
      Console.WriteLine("then");
    else
      Console.WriteLine("else");
    end;
    return 0;
  end Main;
end Program;
)bzl"),
        std::string("else\n"));
}

// A `string` at an `Object` carrier is a bare `bz_string*` (its word 0 is the
// character data, not a vtable). §12.7: a string is an instance of `string`
// and of no other type — so the test must answer `false` and must NOT walk
// the string's data pointer as a descriptor chain (the #1071 / #2793
// SIGSEGV shape). The compiler's own string guard (bz_obj_is_string) is what
// makes this safe.
BEZEL_TEST(codegen_type_test_8000, is_test_on_a_string_carrier_is_false) {
    BEZEL_EXPECT_EQ(
        stdoutOf(R"bzl(module M8000T;
import System.Threading;
public class Program
  public static method Main(): int
    var o: Object := "hello";
    if o is CancellationTokenSource then
      Console.WriteLine("then");
    else
      Console.WriteLine("else");
    end;
    return 0;
  end Main;
end Program;
)bzl"),
        std::string("else\n"));
}

// ── THE #7991 CRASH MUST STILL BE GONE ─────────────────────────────────────
// #7991's review measured that the BASE's general §12.7 narrowing clause
// SEGFAULTED THE COMPILER — a `CreateExtractValue` on the non-aggregate
// `Object` slot (src/codegen/LlvmBackend_Exprs.cpp:6827) — on
// `var o: Object := 5; if o is ReadOnlyStringSpan then … end;`. That crash
// was fixed by #7991 (landed on staging as `bd1f8788`). This fix is in the
// same neighbourhood, so the crash must STILL be gone: the compiler must
// finish, not crash.
//
// ⚠ RE-BASED by fix #7973 (§8.1, v0.350). The original program read `o.Length`
// — a member of `ReadOnlyStringSpan`, NOT of the root `Object` — and §8.1
// ("The declaration above defines the public surface") + §26.5 ("members
// resolve against the DECLARED type") now REFUSE that read, so the program no
// longer reaches codegen at all and the crash guard would be vacuous. The
// shape keeps the carrier, the boxed value, the `is` test against the
// vtable-less builtin CLASS and the member read through the `Object` carrier
// — only the member is now one of §8.1's own (`GetHashCode`), so the program
// still compiles, still runs the type-test lowering, and still exercises the
// path the crash lived on. The REFUSED spelling of the original program is
// pinned by the row below, which also proves (the diagnostic names `Object`,
// not `ReadOnlyStringSpan`) that the narrowing #7991 closed is still closed.
BEZEL_TEST(codegen_type_test_8000,
           boxed_value_is_test_does_not_crash_the_compiler) {
    const CompileOutput out = compileCapturingStderr(R"bzl(module M8000B;
public class Program
  public static method Main(): int
    var o: Object := 5;
    if o is ReadOnlyStringSpan then
      Console.WriteLine(o.GetHashCode());
    end;
    return 0;
  end Main;
end Program;
)bzl");
    BEZEL_EXPECT_TRUE(out.ran);
    // A compiler crash exits with a signal (128 + SIGSEGV = 139) or dies
    // before producing any output; a clean compile exits 0.
    BEZEL_EXPECT_EQ(out.exitCode, 0);
    BEZEL_EXPECT_TRUE(out.text.find("Segmentation fault") == std::string::npos);
}

// ── THE SAME SHAPE, REFUSED BY §8.1 (fix #7973) ────────────────────────────
// The original crash-guard program, verbatim: the carrier is the root
// `Object`, the value is a boxed `int`, the test is against the vtable-less
// builtin CLASS `ReadOnlyStringSpan`, and the member read is `o.Length`.
// `Length` is a member of `ReadOnlyStringSpan` (stdlib §3.3), not of the root
// `Object` (§8.1 lists exactly `GetType`, `Equals`, `GetHashCode`, `ToString`
// and the static `ReferenceEquals`), and §26.5 resolves a member against the
// receiver's DECLARED type — so the read names no member and is a COMPILE
// ERROR.
//
// ⚠ ORACLE. The row asserts the REFUSAL ITSELF — the exit code (1, not a
// signal) AND the diagnostic text — and not the mere absence of a crash: the
// compiler must refuse the program, and it must do so with the §8.1
// diagnostic. The diagnostic naming `Object` (not `ReadOnlyStringSpan`) is
// also what proves the narrowing #7991 closed is STILL closed: had the
// narrowing applied, the member would have resolved against
// `ReadOnlyStringSpan` and there would be nothing to refuse.
BEZEL_TEST(codegen_type_test_8000,
           boxed_value_is_test_member_read_is_refused_spec_8_1_7973) {
    const CompileOutput out = compileCapturingStderr(R"bzl(module M8000R;
public class Program
  public static method Main(): int
    var o: Object := 5;
    if o is ReadOnlyStringSpan then
      Console.WriteLine(o.Length);
    end;
    return 0;
  end Main;
end Program;
)bzl");
    BEZEL_EXPECT_TRUE(out.ran);
    // 1 = a clean refusal; a crash would be 128 + SIGSEGV = 139 (or -1).
    BEZEL_EXPECT_EQ(out.exitCode, 1);
    BEZEL_EXPECT_TRUE(out.text.find("Segmentation fault") == std::string::npos);
    BEZEL_EXPECT_TRUE(out.text.find("no member `Length` on type `Object`") !=
                       std::string::npos);
}

// ── PART (b): THE NEWLY TAGGED CLASSES ─────────────────────────────────────
// narrow #8000 (§12.7 / §19.3 / §8.2, v0.355). Every row below RUNS under
// runAotChecked and FAILED on the base (`6f1e1a15`): the tests read as false
// (or the dispatch through word 0 faulted).

// §12.7: `is not` on a tagged class answers from the same tag. `o` holds a
// Stopwatch, so `o is not Stopwatch` is false and `t is not Stopwatch` (a
// string carrier) is true; `o is Stopwatch` prints True as a value.
BEZEL_TEST(codegen_type_test_8000, b_is_and_is_not_produce_bools_on_stopwatch) {
    BEZEL_EXPECT_EQ(checkedStdoutOf(R"bzl(module M8000BB;
import System.Diagnostics;
public class Program
  public static method Main(): int
    using var w := new Stopwatch();
    var o: Object := w;
    var t: Object := "text";
    Console.WriteLine(o is Stopwatch);
    Console.WriteLine(o is not Stopwatch);
    Console.WriteLine(t is Stopwatch);
    Console.WriteLine(t is not Stopwatch);
    return 0;
  end Main;
end Program;
)bzl"),
                    std::string("True\nFalse\nFalse\nTrue\n"));
}

// §12.7: `as T` yields the value when it is a `T` and null otherwise — a
// string carrier and a Semaphore carrier are both mismatches.
BEZEL_TEST(codegen_type_test_8000, b_as_yields_value_or_null) {
    BEZEL_EXPECT_EQ(checkedStdoutOf(R"bzl(module M8000BA;
import System.Diagnostics;
import System.Threading;
public class Program
  public static method Probe(o: Object): string
    var sw := o as Stopwatch;
    if sw = null then
      return "null";
    end;
    return "sw";
  end Probe;
  public static method Main(): int
    using var w := new Stopwatch();
    Console.WriteLine(Probe(w));
    Console.WriteLine(Probe("text"));
    using var s := new Semaphore(1, 1);
    Console.WriteLine(Probe(s));
    return 0;
  end Main;
end Program;
)bzl"),
                    std::string("sw\nnull\nnull\n"));
}

// §12.7 / §8.2: each of the seven tagged classes is identified as itself and
// as none of the others (a first-match chain), a string carrier matches none
// of them, and a Mutex (tagged before this fix) matches none of them either.
// `o.ToString()` (§8.1) dispatches through the new slot 3 and renders the
// qualified name — on the base it read word 0 as a vtable and faulted.
BEZEL_TEST(codegen_type_test_8000, b_all_seven_tagged_classes_are_distinguished) {
    BEZEL_EXPECT_EQ(checkedStdoutOf(R"bzl(module M8000BK;
import System.Diagnostics;
import System.Threading;
import System.IO;
public class Program
  public static method Kind(o: Object): string
    if o is Stopwatch then return "Stopwatch"; end;
    if o is Random then return "Random"; end;
    if o is FileStream then return "FileStream"; end;
    if o is Lock then return "Lock"; end;
    if o is Semaphore then return "Semaphore"; end;
    if o is SemaphoreToken then return "SemaphoreToken"; end;
    if o is TaskScope then return "TaskScope"; end;
    if o is string then return "string"; end;
    return "other";
  end Kind;
  public static method Show(o: Object)
    Console.WriteLine(Kind(o) + " " + o.ToString());
  end Show;
  public static async method Main(): Task<int>
    using var sw := new Stopwatch();
    Show(sw);
    using var r := new Random(3);
    Show(r);
    using var fs := new FileStream("/var/tmp/bezel-scratch/bezel_8000_kinds.txt", FileMode.Create);
    Show(fs);
    using var m := new Mutex();
    using var l := await m.AcquireAsync();
    Show(l);
    using var s := new Semaphore(1, 1);
    Show(s);
    using var t := await s.AcquireAsync();
    Show(t);
    await using var scope := new TaskScope();
    Show(scope);
    await scope.Join();
    Show("hi");
    Show(m);
    return 0;
  end Main;
end Program;
)bzl"),
                    std::string("Stopwatch System.Diagnostics.Stopwatch\n"
                                "Random System.Random\n"
                                "FileStream System.IO.FileStream\n"
                                "Lock System.Threading.Lock\n"
                                "Semaphore System.Threading.Semaphore\n"
                                "SemaphoreToken System.Threading.SemaphoreToken\n"
                                "TaskScope System.Threading.TaskScope\n"
                                "string hi\n"
                                "other System.Threading.Mutex\n"));
}

// §28.4 / §12.6: an owned value at an `IDisposable` slot is discharged by the
// class protocol (slot-2 Dispose, then reclaim). On the base slot 2 was read
// through a word-0 DATA pointer — a wild jump (SIGSEGV) for every one of
// these. Each is now disposed exactly once and reclaimed (LSan-clean); the
// Semaphore token and the Mutex lock release their permit / lock, so the
// owner's own Dispose after them is clean.
BEZEL_TEST(codegen_type_test_8000, b_owned_idisposable_slot_disposes_tagged_classes) {
    BEZEL_EXPECT_EQ(checkedStdoutOf(R"bzl(module M8000BD;
import System.Diagnostics;
import System.Threading;
import System.IO;
public class Program
  public static method Take(owned d: IDisposable)
    d.Dispose();
  end Take;
  public static method Main(): int
    Take(Stopwatch.StartNew());
    Take(new Random(3));
    Take(new Semaphore(1, 1));
    var s := new Semaphore(1, 1);
    Take(s.Acquire());
    Take(s.Acquire());
    s.Dispose();
    var m := new Mutex();
    Take(m.Acquire());
    Take(m.Acquire());
    m.Dispose();
    Take(new FileStream("/var/tmp/bezel-scratch/bezel_8000_take.txt", FileMode.Create));
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
)bzl"),
                    std::string("ok\n"));
}

// A 100x loop (the one-shot-leak trap): each iteration allocates a tagged
// Stopwatch and Random, tests them through an `Object` carrier, and disposes
// them; runAotChecked's LSan leg fails the row on any leaked cell.
BEZEL_TEST(codegen_type_test_8000, b_loop_100x_is_leak_free) {
    BEZEL_EXPECT_EQ(checkedStdoutOf(R"bzl(module M8000BL;
import System.Diagnostics;
public class Program
  public static method Main(): int
    var hits := 0;
    var i := 0;
    while i < 100 do
      using var x := Stopwatch.StartNew();
      var o: Object := x;
      if o is Stopwatch then
        o.Stop();
        hits := hits + 1;
      end;
      using var r := new Random(i);
      var q: Object := r;
      if q is Random then hits := hits + 1; end;
      i := i + 1;
    end;
    Console.WriteLine(hits);
    return 0;
  end Main;
end Program;
)bzl"),
                    std::string("200\n"));
}

// ── THE RESIDUAL: A CLASS STILL WITHOUT A TAG IS STILL DIAGNOSED ───────────
// `CancellationToken` (stdlib §6.1) has no runtime tag yet, so `o is
// CancellationToken` at an `Object` carrier still takes the (c) bail, and the
// drop must stay LOUD. This row is also what keeps the absence asserted by
// `is_test_on_vtableless_builtin_is_answered` non-vacuous: the same needle is
// still produced by the same compile path.
BEZEL_TEST(codegen_type_test_8000, residual_class_drop_is_still_diagnosed) {
    const CompileOutput out = compileCapturingStderr(R"bzl(module M8000RC;
import System.Threading;
public class Program
  public static method Main(): int
    using var cts := new CancellationTokenSource();
    var o: Object := cts.Token;
    if o is CancellationToken then
      Console.WriteLine("tok");
    end;
    return 0;
  end Main;
end Program;
)bzl");
    BEZEL_EXPECT_TRUE(out.ran);
    BEZEL_EXPECT_EQ(out.exitCode, 0);
    BEZEL_EXPECT_TRUE(out.text.find(kDroppedClassNeedle) != std::string::npos);
    BEZEL_EXPECT_TRUE(out.text.find("`CancellationToken`") != std::string::npos);
    BEZEL_EXPECT_TRUE(out.text.find("#8000") != std::string::npos);
    BEZEL_EXPECT_TRUE(out.text.find(":0:0") == std::string::npos);
}
