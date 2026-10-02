// Codegen / refcounting — issue #5245: a `string?` local that BORROWS a class
// field (`var s: string? := c.Val;`) took no +1, so it dangled when the field
// was stored over (`c.Set(...)`) and the next read of `s` was a use-after-free
// (SEGV in bz_string_to_utf8z / heap-use-after-free under ASan). The
// non-nullable `string` twin was always clean.
//
// Spec: §4.6 "`string`" and §28.9 "Runtime-managed types" ("`string` ... created
// by literals, interpolation, concatenation, and library methods"; the
// reference-counting implementation is not specified), §5.2 nullable types and
// §6.6 `??=` for the nullable shapes below, §28.3 K1.6 (runtime-managed values
// carry no obligation). Spec revision v0.313. The observable contract is
// §28.9's: a binding that names a `string` value keeps it alive; here the local
// `s` must still read the value it was bound to after the field is overwritten.
//
// THE DEFECT. collectStringReleaseEligible admitted only NON-nullable borrow
// initializers to stringBorrowAll_ (the set armStringLocalRefcount retains at
// bind), and lowerStringLocalReassign declined a nullable target. The fix
// admits `string?` in BOTH, together: bind-time retain + scope-end release, and
// retain-new / release-old on `:=`, plus the `??=` store, plus
// returnedStringLocalOwnsPlusOne no longer claims the +1 of a borrow local the
// scope-end release also frees (`s ?? d`).
//
// Every row is loop-amplified (50 iterations), builds strings at RUNTIME (a
// literal is an rc sentinel that hides the bug) and asserts the exact stdout.
// runAotChecked = generated-code ASan + LeakSanitizer.
//
// KNOWN, DELIBERATE LEAK (lambda row). The closure env-destructor releases the
// captured string only for a NON-nullable `string` (isStringTypeRefNonNull on
// both the capture retain and the envdtor release), so a `string?` local
// captured by a lambda keeps its bind-time +1 and leaks it (safe direction:
// it used to be a use-after-free). That row therefore runs under
// runAotCheckedLenient (ASan UAF net on, LSan off) and pins the stdout only.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked / runAotCheckedLenient
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

constexpr int kIters = 50;

// `C.Val` reads the field, `C.Set` stores over it (releasing the old string).
const char* kHdr =
    "module M; "
    "public class C "
    "  private _v: string; "
    "  public constructor(v: string) _v := v; end constructor; "
    "  public property Val: string => _v; "
    "  public method Set(x: string) _v := x; end Set; "
    "end C; ";

std::string prog(const std::string& body) {
    return std::string(kHdr) + body;
}

std::string mainLoop(const std::string& loopBody) {
    return "public class Program "
           "  public static method Main(): int "
           "    using var c := new C(\"a\" + 1.ToString()); "
           "    for var i in 0..<50 do " + loopBody +
           "    end; "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// The value the field held BEFORE iteration i's `c.Set("<tag><i>")`.
std::string prevVal(int i, const char* tag) {
    return i == 0 ? std::string("a1") : std::string(tag) + std::to_string(i - 1);
}

std::string run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// (1) The issue's own repro: `s` aliases the field, the field is stored over,
// `s` is read. Pre-fix: SEGV on the first read.
BEZEL_TEST(string_nullable_borrow_local_release_5245,
           repro_nullable_local_survives_field_overwrite_spec_4_6_28_9) {
    std::string want;
    for (int i = 0; i < kIters; ++i) want += prevVal(i, "q") + "\n";
    BEZEL_EXPECT_EQ(run(prog(mainLoop(
        "var s: string? := c.Val; "
        "c.Set(\"q\" + i.ToString()); "
        "Console.WriteLine(s ?? \"none\"); "))), want);
}

// (2) Reassign to a non-null borrow, to null, to a +1 producer; every displaced
// value is released exactly once and every read is live.
BEZEL_TEST(string_nullable_borrow_local_release_5245,
           reassign_borrow_null_producer_spec_4_6_28_9) {
    std::string want;
    for (int i = 0; i < kIters; ++i) {
        want += "q" + std::to_string(i) + "\n";
        want += "none\n";
        want += "z" + std::to_string(i) + "\n";
    }
    BEZEL_EXPECT_EQ(run(prog(mainLoop(
        "var s: string? := c.Val; "
        "c.Set(\"q\" + i.ToString()); "
        "s := c.Val; "
        "c.Set(\"r\" + i.ToString()); "
        "Console.WriteLine(s ?? \"none\"); "
        "s := null; "
        "Console.WriteLine(s ?? \"none\"); "
        "s := \"z\" + i.ToString(); "
        "Console.WriteLine(s ?? \"none\"); "))), want);
}

// (3) A null-initialized local later given a producer (null path then store).
BEZEL_TEST(string_nullable_borrow_local_release_5245,
           null_init_then_conditional_producer_spec_4_6_28_9) {
    std::string want;
    for (int i = 0; i < kIters; ++i)
        want += (i % 2 == 0 ? "x" + std::to_string(i) : std::string("none")) +
                "\n";
    BEZEL_EXPECT_EQ(run(prog(mainLoop(
        "var s: string? := null; "
        "if i mod 2 = 0 then s := \"x\" + i.ToString(); end; "
        "Console.WriteLine(s ?? \"none\"); "))), want);
}

// (4) A producer-initialized `string?` local reassigned to a field borrow:
// the store must retain (the scope-end release now frees the borrow).
BEZEL_TEST(string_nullable_borrow_local_release_5245,
           producer_local_reassigned_to_borrow_spec_4_6_28_9) {
    std::string want;
    for (int i = 0; i < kIters; ++i) want += prevVal(i, "q") + "\n";
    BEZEL_EXPECT_EQ(run(prog(mainLoop(
        "var s: string? := \"p\" + i.ToString(); "
        "s := c.Val; "
        "c.Set(\"q\" + i.ToString()); "
        "Console.WriteLine(s ?? \"none\"); "))), want);
}

// (5) `??=` into a null `string?` local stores a field borrow: it must take its
// own +1 (§6.6: assigned only when the current value is null).
BEZEL_TEST(string_nullable_borrow_local_release_5245,
           coalesce_assign_stores_borrow_spec_6_6_28_9) {
    std::string want;
    for (int i = 0; i < kIters; ++i) want += prevVal(i, "q") + "\n";
    BEZEL_EXPECT_EQ(run(prog(mainLoop(
        "var s: string? := null; "
        "s ??= c.Val; "
        "c.Set(\"q\" + i.ToString()); "
        "Console.WriteLine(s ?? \"none\"); "))), want);
}

// (6) `??=` on a NON-null local is a no-op (short-circuit), the borrow keeps its
// own +1.
BEZEL_TEST(string_nullable_borrow_local_release_5245,
           coalesce_assign_nonnull_is_noop_spec_6_6_28_9) {
    std::string want;
    for (int i = 0; i < kIters; ++i) want += prevVal(i, "q") + "\n";
    BEZEL_EXPECT_EQ(run(prog(mainLoop(
        "var s: string? := c.Val; "
        "s ??= \"dflt\"; "
        "c.Set(\"q\" + i.ToString()); "
        "Console.WriteLine(s ?? \"none\"); "))), want);
}

// (7) Narrowing after `<> null`: both reads of `s` happen after the overwrite.
BEZEL_TEST(string_nullable_borrow_local_release_5245,
           narrowed_reads_after_overwrite_spec_5_2_28_9) {
    std::string want;
    for (int i = 0; i < kIters; ++i) {
        const std::string v = prevVal(i, "q");
        want += std::to_string(v.size()) + "\n" + v + "\n";
    }
    BEZEL_EXPECT_EQ(run(prog(mainLoop(
        "var s: string? := c.Val; "
        "c.Set(\"q\" + i.ToString()); "
        "if s <> null then "
        "  Console.WriteLine(s.Length); Console.WriteLine(s); "
        "end; "))), want);
}

// (8) Returning the local: the +1 it holds transfers to the caller, which then
// outlives the field's next overwrite.
BEZEL_TEST(string_nullable_borrow_local_release_5245,
           returned_nullable_local_spec_4_6_28_9) {
    std::string want;
    for (int i = 0; i < kIters; ++i) want += prevVal(i, "w") + "\n";
    BEZEL_EXPECT_EQ(run(prog(
        "public class Program "
        "  static method Get(c: C, i: int): string? "
        "    var s: string? := c.Val; "
        "    c.Set(\"q\" + i.ToString()); "
        "    return s; "
        "  end Get; "
        "  public static method Main(): int "
        "    using var c := new C(\"a\" + 1.ToString()); "
        "    for var i in 0..<50 do "
        "      var r := Get(c, i); "
        "      c.Set(\"w\" + i.ToString()); "
        "      Console.WriteLine(r ?? \"none\"); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;")), want);
}

// (9) Passing the local as an argument while the callee overwrites the field.
BEZEL_TEST(string_nullable_borrow_local_release_5245,
           passed_as_argument_spec_4_6_28_9) {
    std::string want;
    for (int i = 0; i < kIters; ++i) {
        want += prevVal(i, "q") + "\n";   // inside Show
        want += prevVal(i, "q") + "\n";   // after Show, in Main
    }
    BEZEL_EXPECT_EQ(run(prog(
        "public class Program "
        "  static method Show(c: C, t: string?, i: int) "
        "    c.Set(\"q\" + i.ToString()); "
        "    Console.WriteLine(t ?? \"none\"); "
        "  end Show; "
        "  public static method Main(): int "
        "    using var c := new C(\"a\" + 1.ToString()); "
        "    for var i in 0..<50 do "
        "      var s: string? := c.Val; "
        "      Show(c, s, i); "
        "      Console.WriteLine(s ?? \"none\"); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;")), want);
}

// (10) Async frame: the local lives across an `await` and a field overwrite.
BEZEL_TEST(string_nullable_borrow_local_release_5245,
           async_frame_nullable_local_spec_4_6_28_9_30) {
    std::string want;
    for (int i = 0; i < kIters; ++i) {
        want += prevVal(i, "r") + "\n";
        want += "q" + std::to_string(i) + "\n";
    }
    BEZEL_EXPECT_EQ(run(prog(
        "public class Program "
        "  public static async method Run(c: C, i: int): Task "
        "    var s: string? := c.Val; "
        "    await Task.Delay(0); "
        "    c.Set(\"q\" + i.ToString()); "
        "    Console.WriteLine(s ?? \"none\"); "
        "    s := c.Val; "
        "    c.Set(\"r\" + i.ToString()); "
        "    Console.WriteLine(s ?? \"none\"); "
        "  end Run; "
        "  public static method Main(): int "
        "    using var c := new C(\"a\" + 1.ToString()); "
        "    for var i in 0..<50 do "
        "      Run(c, i).Wait(); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;")), want);
}

// (11) Lambda capture. KNOWN LEAK, see the file header: Lenient = ASan on, LSan
// off, stdout pinned. Pre-fix this was a use-after-free (SEGV).
BEZEL_TEST(string_nullable_borrow_local_release_5245,
           lambda_capture_reads_live_value_spec_27_5_28_9) {
    std::string want;
    for (int i = 0; i < kIters; ++i) want += prevVal(i, "q") + "\n";
    auto r = runAotCheckedLenient(prog(mainLoop(
        "var s: string? := c.Val; "
        "var f := lambda() => s ?? \"none\"; "
        "c.Set(\"q\" + i.ToString()); "
        "Console.WriteLine(f()); ")));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

// (12) The consumer half of the fix, on a NON-nullable borrow local: `n ?? p`
// reads `p` as a safe read (so `p` is released at scope end) and used to skip
// the retain it owed the result (returnedStringLocalOwnsPlusOne claimed the
// local's +1 although the scope-end release also frees it): SEGV pre-fix.
BEZEL_TEST(string_nullable_borrow_local_release_5245,
           coalesce_rhs_borrow_local_mints_own_retain_spec_7_7_28_9) {
    std::string want;
    for (int i = 0; i < 3; ++i) want += "a1\n";
    BEZEL_EXPECT_EQ(run(prog(
        "public class Program "
        "  static method Pick(n: string?, c: C): string "
        "    var p: string := c.Val; "
        "    var r := n ?? p; "
        "    return r; "
        "  end Pick; "
        "  public static method Main(): int "
        "    using var c := new C(\"a\" + 1.ToString()); "
        "    for var i in 0..<3 do "
        "      Console.WriteLine(Pick(null, c)); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Program;")), want);
}
