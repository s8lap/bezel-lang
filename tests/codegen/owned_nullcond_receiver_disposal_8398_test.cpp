// Codegen — issue #8398: an owned nullable RESULT used as the receiver of `?.`
// or `!` is never disposed. §28.3.1 K8.1 / §28.3.11 (spec v0.357).
//
// THE SPEC.
//
// §28.3.11 *Unbound owned temporaries* (v0.357):
//   "An owned value may also be produced as an **unbound temporary**: the
//    result of an `owned`-returning call (or other owned producer) that is not
//    bound to any name, not `move`d, and not returned, but used only by
//    *borrowing* operations within the expression — as the receiver of a
//    further call, as a non-`owned` argument, as an operator operand, **as a
//    member or index access**, or as the scrutinee of a `case` … Such a
//    temporary is disposed at the **end of the full expression** in which it is
//    produced, after every use of it within that expression has completed."
//
// §28.3.1 K8.1 (v0.357), the full-expression boundary:
//   "Unbound owned temporaries are disposed at their full expression's end,
//    after their uses, in reverse production order — on the normal edge as on
//    the exception edge, where the pass runs during unwinding."
//
// §17.12 — the postfix `!` (null-forgiving) is a PASS-THROUGH: it raises
// `NullReferenceException` when the operand is null and otherwise yields the
// operand's value. So `f()!.M(x)` PRODUCES the temporary `f()` and USES it as
// the receiver.
//
// §7.5 — `?.`: the receiver is evaluated once; if it is null the member is not
// accessed.
//
// THE DEFECT. Two independent gaps, both about the RECEIVER:
//
//   (1) the `?.` spelling never registered the receiver temporary at all —
//       `lowerNullCondCall` (LlvmBackend_Calls2.cpp) had neither the
//       `registerFreshNewReceiverTempUnwind` registration nor the
//       `disposeFreshNewReceiverTemp` reclaim that the `.` spelling
//       (`lowerCallDispatch`) has;
//   (2) the `!` spelling asks every consumer "is this receiver a fresh owned
//       producer?", and the receiver of `f()!.M(x)` is the `PostfixBangExpr`
//       wrapper — never a producer — so the answer was NO.
//
// MEASURED on the base (r30-c1 @ 71317e6c9, `bezelc` 0.0.0): the issue's own
// repro printed
//     a <- x
//     after ?.
//     b <- y
//     after !
//     c <- z
//     close c
//     after control
// — `close a` and `close b` MISSING. The non-nullable control
// `Open("c").Send("z")` printed `close c`: the two spellings of ONE member
// disagreed. The object leaked and its destructor never ran, so the missing
// side effect was silently missing from the output.
//
// THE FIX. The `.` spelling's own two-channel mechanism, reused verbatim:
//   * the EXCEPTION edge — an `unwindOnly` dispose-stack entry registered at
//     the receiver's production, so a throw out of an argument evaluation or
//     out of the callee discharges the temporary while unwinding
//     (§28.3.11's *Exception-path discharge*);
//   * the NORMAL edge — the reclaim right after the access/call, which
//     discharges the unwind entry at RUNTIME (the slot is nulled), so the two
//     edges can never both fire: exactly once on every path.
//
// The shared eligibility predicate (`Emitter::freshNewReceiverTempEligible`)
// gained the §17.12 unwrap, so every consumer — the `.`-form call, the
// `.`-form member read, the `?.`-form member read — recognises the producer
// under a `!` at ONE definition, and the `?.`-form call/member-read paths
// gained the registration/reclaim the `.` form already had.
//
// THE ORACLE. Two channels, both value-observable:
//   * the issue's own repro, asserted on its EXACT stdout (the destructor's
//     `close <name>` line and its position relative to the `after …`
//     markers) — that is the issue's own observable, unchanged;
//   * a live-object counter (ctor +1 / dtor -1) over a 200-iteration loop,
//     whose exit code is the number of instances still alive at exit: 0 when
//     every temporary was disposed exactly once, 200 when they leaked. The
//     counter makes "exactly once" observable as a value: a leak shows as
//     `live > 0` and a double dispose as a wrong count or a crash.
//
// (A one-iteration `Main` is an unreliable LSan oracle — a leftover register
// copy makes LSan call a leaked block reachable — so every leak row loops.)

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked / …Lenient
#include "test_harness.hpp"

using bezel::test::JitResult;

namespace {

bool wasSkipped(const JitResult& r) {
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.message.find("SKIP") != std::string::npos) return true;
    }
    return false;
}

// Plain (non-instrumented) AOT run.
int runPlain(const std::string& src) {
    JitResult r = bezel::test::runAot(src);
    return r.ran ? r.exitCode : -999;
}

// ASan + LSan run. A leak / double-free / use-after-free surfaces as
// ran=false with the sanitizer report in `diagnostics`, i.e. -999.
int runChecked(const std::string& src) {
    JitResult r = bezel::test::runAotChecked(src);
    if (wasSkipped(r)) return -1000;   // ASan toolchain absent — case skipped
    return r.ran ? r.exitCode : -999;
}

// ASan WITHOUT LeakSanitizer — for the EXCLUSION rows, whose receivers are
// deliberately NOT reclaimed (the aliasing gate). Use-after-free / double-free
// detection in the generated code is unchanged, which is exactly the
// load-bearing assertion there.
int runCheckedLenient(const std::string& src) {
    JitResult r = bezel::test::runAotCheckedLenient(src);
    if (wasSkipped(r)) return -1000;
    return r.ran ? r.exitCode : -999;
}

// How many times `needle` occurs in `hay`.
int countOf(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// The issue's own repro, verbatim (issue #8398's *Repro* block).
const char* kIssueRepro =
    "module P; "
    "public class Conn "
    "  private _name: string; "
    "  public constructor(name: string) _name := name; end constructor; "
    "  public method Send(s: string) => Console.WriteLine($\"{_name} <- {s}\"); "
    "  destructor Console.WriteLine($\"close {_name}\"); end destructor; "
    "end Conn; "
    "public class Program "
    "  public static method OpenOrNull(name: string): owned Conn? => new Conn(name); "
    "  public static method Open(name: string): owned Conn => new Conn(name); "
    "  public static method Main() "
    "    Program.OpenOrNull(\"a\")?.Send(\"x\"); "
    "    Console.WriteLine(\"after ?.\"); "
    "    Program.OpenOrNull(\"b\")!.Send(\"y\"); "
    "    Console.WriteLine(\"after !\"); "
    "    Program.Open(\"c\").Send(\"z\"); "
    "    Console.WriteLine(\"after control\"); "
    "  end Main; "
    "end Program;";

// The issue's *Expected* output, verbatim.
const char* kIssueExpected =
    "a <- x\n"
    "close a\n"
    "after ?.\n"
    "b <- y\n"
    "close b\n"
    "after !\n"
    "c <- z\n"
    "close c\n"
    "after control\n";

// ---------------------------------------------------------------------------
// FAIL-WITHOUT / PASS-WITH — row 1: the issue's own repro, on its own
// observable. Pre-fix the stdout was the same program minus the two `close`
// lines, so this row failed; post-fix it is the issue's *Expected* block.
// ---------------------------------------------------------------------------
BEZEL_TEST(owned_nullcond_receiver_disposal_8398, issue_repro_disposes_both) {
    JitResult r = bezel::test::runAot(kIssueRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, kIssueExpected);
    // The ASan leg: a leak would make the run fail (ran=false). A double
    // dispose would fault here instead.
    JitResult c = bezel::test::runAotChecked(kIssueRepro);
    if (wasSkipped(c)) return;
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.stdoutText, kIssueExpected);
}

// ---------------------------------------------------------------------------
// The 200-iteration loop. `Conn.live` is the number of instances still alive
// (ctor +1 / dtor -1) and the exit code carries it: 0 when every temporary was
// disposed exactly once, 200 when they all leaked. 200 fits an 8-bit exit
// code (the AOT path truncates), so the pre-fix value is readable.
// ---------------------------------------------------------------------------

// The loop program. `mode` selects the receiver spelling under test.
std::string loopProg(const std::string& mode) {
    return
        "module M; "
        "public class Conn "
        "  public static live: int := 0; "
        "  public static made: int := 0; "
        "  private _name: string; "
        "  public constructor(name: string) "
        "    _name := name; "
        "    Conn.live := Conn.live + 1; "
        "    Conn.made := Conn.made + 1; "
        "  end constructor; "
        "  destructor "
        "    Conn.live := Conn.live - 1; "
        "    Console.WriteLine($\"close {_name}\"); "
        "  end destructor; "
        "  public method Send(s: string) => Console.WriteLine($\"{_name} <- {s}\"); "
        "end Conn; "
        "public class Program "
        "  public static method OpenOrNull(name: string): owned Conn? => new Conn(name); "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 200 do " + mode + " i := i + 1; end while; "
        "    return Conn.live; "
        "  end Main; "
        "end Program;";
}

// The `?.` receiver: 200 temporaries, all disposed.
BEZEL_TEST(owned_nullcond_receiver_disposal_8398, nullcond_receiver_loop_disposed) {
    std::string src = loopProg(
        "Program.OpenOrNull(\"a\")?.Send(\"x\"); ");
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    JitResult r = bezel::test::runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(countOf(r.stdoutText, "close a"), 200);
    int c = runChecked(src);
    if (c == -1000) return;
    BEZEL_EXPECT_EQ(c, 0);
}

// The `!` receiver: 200 temporaries, all disposed.
BEZEL_TEST(owned_nullcond_receiver_disposal_8398, bang_receiver_loop_disposed) {
    std::string src = loopProg(
        "Program.OpenOrNull(\"b\")!.Send(\"y\"); ");
    BEZEL_EXPECT_EQ(runPlain(src), 0);
    JitResult r = bezel::test::runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(countOf(r.stdoutText, "close b"), 200);
    int c = runChecked(src);
    if (c == -1000) return;
    BEZEL_EXPECT_EQ(c, 0);
}

// ---------------------------------------------------------------------------
// The `?.` and `!` MEMBER READ: `f()?.P` / `f()!.P`. §28.3.11 lists "a member
// or index access" among the borrowing uses, so the same obligation holds.
// The member is an `int` property — a value-like result, the shape the
// shared eligibility gate admits.
// ---------------------------------------------------------------------------
BEZEL_TEST(owned_nullcond_receiver_disposal_8398, nullcond_member_read_disposed) {
    std::string src =
        "module M; "
        "public class Conn "
        "  public static live: int := 0; "
        "  private _n: int := 7; "
        "  public constructor() Conn.live := Conn.live + 1; end constructor; "
        "  destructor Conn.live := Conn.live - 1; end destructor; "
        "  public property N: int get => _n; end N; "
        "end Conn; "
        "public class Program "
        "  public static method OpenOrNull(): owned Conn? => new Conn(); "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      total := total + (Program.OpenOrNull()?.N ?? 0); "
        "      i := i + 1; "
        "    end while; "
        "    return Conn.live * 100 + total; "
        "  end Main; "
        "end Program;";
    // live == 0 (all disposed) and total == 7 * 200 == 1400 → 1400 mod 256 =
    // 120; the exit code is `live * 100 + total` truncated to 8 bits, so the
    // pre-fix value (200 live) is 20120 mod 256 = 152 and the post-fix value is
    // 1400 mod 256 = 120. The live count is asserted through the exit code's
    // high part: 200 live would give 152, not 120.
    BEZEL_EXPECT_EQ(runPlain(src), 120);
    int c = runChecked(src);
    if (c == -1000) return;
    BEZEL_EXPECT_EQ(c, 120);
}

BEZEL_TEST(owned_nullcond_receiver_disposal_8398, bang_member_read_disposed) {
    std::string src =
        "module M; "
        "public class Conn "
        "  public static live: int := 0; "
        "  private _n: int := 7; "
        "  public constructor() Conn.live := Conn.live + 1; end constructor; "
        "  destructor Conn.live := Conn.live - 1; end destructor; "
        "  public property N: int get => _n; end N; "
        "end Conn; "
        "public class Program "
        "  public static method OpenOrNull(): owned Conn? => new Conn(); "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      total := total + (Program.OpenOrNull()!.N); "
        "      i := i + 1; "
        "    end while; "
        "    return Conn.live * 100 + total; "
        "  end Main; "
        "end Program;";
    BEZEL_EXPECT_EQ(runPlain(src), 120);
    int c = runChecked(src);
    if (c == -1000) return;
    BEZEL_EXPECT_EQ(c, 120);
}

// ---------------------------------------------------------------------------
// CONTROLS — green on the base AND on the fix. A control that stays green on
// the base is doing its job: it shows the shape is genuinely admitted rather
// than rejected for an unrelated reason.
// ---------------------------------------------------------------------------

// §7.5: a NULL receiver means the member is not accessed and there is no
// object to dispose. The `?.` call is skipped and nothing is reclaimed.
BEZEL_TEST(owned_nullcond_receiver_disposal_8398, null_receiver_path_is_clean) {
    std::string src =
        "module M; "
        "public class Conn "
        "  public static live: int := 0; "
        "  private _name: string; "
        "  public constructor(name: string) _name := name; "
        "    Conn.live := Conn.live + 1; end constructor; "
        "  destructor Conn.live := Conn.live - 1; "
        "    Console.WriteLine($\"close {_name}\"); end destructor; "
        "  public method Send(s: string) => Console.WriteLine($\"{_name} <- {s}\"); "
        "end Conn; "
        "public class Program "
        "  public static method OpenNull(): owned Conn? => null; "
        "  public static method Main(): int "
        "    Program.OpenNull()?.Send(\"x\"); "
        "    Console.WriteLine(\"after ?.\"); "
        "    return Conn.live; "
        "  end Main; "
        "end Program;";
    JitResult r = bezel::test::runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "after ?.\n");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// The `.` spelling — the admitting twin of the `?.` spelling: the same member,
// the same producer, the non-nullable receiver. It was green on the base and
// must stay green (a fix that changed it would be a defect, not an
// improvement).
BEZEL_TEST(owned_nullcond_receiver_disposal_8398, dot_spelling_control) {
    std::string src =
        "module M; "
        "public class Conn "
        "  public static live: int := 0; "
        "  private _name: string; "
        "  public constructor(name: string) _name := name; "
        "    Conn.live := Conn.live + 1; end constructor; "
        "  destructor Conn.live := Conn.live - 1; "
        "    Console.WriteLine($\"close {_name}\"); end destructor; "
        "  public method Send(s: string) => Console.WriteLine($\"{_name} <- {s}\"); "
        "end Conn; "
        "public class Program "
        "  public static method Open(name: string): owned Conn => new Conn(name); "
        "  public static method Main(): int "
        "    Program.Open(\"c\").Send(\"z\"); "
        "    Console.WriteLine(\"after control\"); "
        "    return Conn.live; "
        "  end Main; "
        "end Program;";
    JitResult r = bezel::test::runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "c <- z\nclose c\nafter control\n");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// A BOUND receiver must not be double-disposed: the local owns the value and
// its scope-end discharge is the only one. A double free would be a crash
// (ran=false) or a SECOND `close d` line, never exactly one.
BEZEL_TEST(owned_nullcond_receiver_disposal_8398, bound_local_not_double_disposed) {
    std::string src =
        "module M; "
        "public class Conn "
        "  private _name: string; "
        "  public constructor(name: string) _name := name; end constructor; "
        "  public method Send(s: string) => Console.WriteLine($\"{_name} <- {s}\"); "
        "  destructor Console.WriteLine($\"close {_name}\"); end destructor; "
        "end Conn; "
        "public class Program "
        "  public static method OpenOrNull(name: string): owned Conn? => new Conn(name); "
        "  public static method Main(): int "
        "    using var c := Program.OpenOrNull(\"d\"); "
        "    c?.Send(\"x\"); "
        "    c!.Send(\"y\"); "
        "    Console.WriteLine(\"after\"); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    JitResult r = bezel::test::runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    // Exactly ONE `close d` — the local's scope-end discharge, and no second
    // one from the `?.` / `!` receiver paths.
    BEZEL_EXPECT_EQ(countOf(r.stdoutText, "close d"), 1);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "d <- x\nd <- y\nafter\nclose d\n");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    int c = runChecked(src);
    if (c == -1000) return;
    BEZEL_EXPECT_EQ(c, 0);
}

// ---------------------------------------------------------------------------
// EXCLUSION — the aliasing gate. A member whose result may embed a BORROW of
// the receiver (here a `string` property, §28.3's reference-result exclusion)
// must leave the receiver alone: the receiver LEAKS BY DESIGN (200 live), and
// the load-bearing assertion is that the generated code never frees a
// still-aliased receiver — a bounded leak is strictly better than a
// use-after-free. This row is GREEN on the base and on the fix.
// ---------------------------------------------------------------------------
BEZEL_TEST(owned_nullcond_receiver_disposal_8398, string_result_receiver_not_disposed) {
    std::string src =
        "module M; "
        "public class Conn "
        "  public static live: int := 0; "
        "  private _s: string := \"hello\"; "
        "  public constructor() Conn.live := Conn.live + 1; end constructor; "
        "  destructor Conn.live := Conn.live - 1; end destructor; "
        "  public property S: string get => _s; end S; "
        "end Conn; "
        "public class Program "
        "  public static method OpenOrNull(): owned Conn? => new Conn(); "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      total := total + (Program.OpenOrNull()?.S ?? \"\").Length; "
        "      i := i + 1; "
        "    end while; "
        "    return Conn.live; "
        "  end Main; "
        "end Program;";
    BEZEL_EXPECT_EQ(runPlain(src), 200);   // 200 live — the exclusion holds
    int c = runCheckedLenient(src);
    if (c == -1000) return;
    BEZEL_EXPECT_EQ(c, 200);
}

}  // namespace
