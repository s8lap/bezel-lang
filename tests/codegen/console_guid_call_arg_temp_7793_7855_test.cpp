// fix #7793 / #7855 / #7803 — a FRESH class-typed, OWNED or CLOSURE temporary
// passed as a CALL ARGUMENT to a BUILT-IN callee is never disposed: the
// instance (the array, the closure cell) is owned by NOBODY and leaks.
//
// ── THE DEFECT ──────────────────────────────────────────────────────────────
//
// §28.3.11 (spec line 7712) — *Unbound owned temporaries*:
//
//   "An owned value may also be produced as an **unbound temporary**: the
//    result of an `owned`-returning call (or other owned producer) that is not
//    bound to any name, not `move`d, and not returned … but used only by
//    *borrowing* operations within the expression — as the receiver of a
//    further call, **as a non-`owned` argument**, … Such a temporary is
//    disposed at the **end of the full expression** in which it is produced,
//    after every use of it within that expression has completed."
//
// and §28.3 (spec line 7759) — *A callee that stores a plain argument*:
//
//   "A non-`owned` argument position is a borrowing use whatever the callee's
//    body does with the parameter, and the temporary's disposal at its
//    boundary does not depend on that body."
//
// ⇒ ⇒ A `new C(...)` (or an `owned C`-returning call) passed as a PLAIN
// argument is an unbound owned temporary, and a fresh `owned Array<T>`
// materializer result passed as a plain argument is one too. Both are owed a
// disposal at the K8 (end-of-full-expression) boundary.
//
// MEASURED on this tree's parent (`bezelc --sanitize=address`, LSan):
//
//   `Console.WriteLine(new Cls())`        → 3200 B / 200 allocations
//                                            (16 B per call — the instance)
//   `Console.Out.WriteLine(new Cls())`    → 3200 B / 200 allocations
//   `Guid.FromByteArray(g.ToByteArray())`→ 19200 B / 400 allocations
//                                            (the array header + its 16-byte
//                                            buffer per call)
//   `Guid.FromByteArray(Encoding.UTF8.GetBytes(s))` → 14400 B / 300 allocations
//
// The NAMED-LOCAL arms of the same two shapes are CLEAN — the local's own
// discharge owns that value — so the difference between the two spellings was
// the whole defect: the BUILT-IN call path lowers its argument directly and
// registers no disposal for it, while the USER-call path does.
//
// §28.3.11's own worked example is this exact shape (spec line 7728): "in
// `Folded(src).Sorted()` where `Folded(src): owned Array<T>` and `Sorted`
// borrows its receiver, the `Folded(src)` temporary is borrowed by `Sorted`,
// then disposed once the statement completes — exactly once, with no leak and
// no double-free. **Disposal runs *after* the borrowing use**, so the receiver
// is live for the duration of the call."
//
// ── THE ORACLE ──────────────────────────────────────────────────────────────
//
// Every leak row runs under `runAotChecked`, which arms AddressSanitizer over
// the GENERATED module AND LeakSanitizer at exit (`ASAN_OPTIONS=
// detect_leaks=1`), and turns either into an error diagnostic with `ran`
// false. A leak therefore fails the row.
//
// ⚠ EVERY LEAK ROW IS LOOPED (200 iterations). A one-shot repro can leave the
// temporary's pointer in a live stack slot at exit and evade LSan's
// reachability scan — this repo has been bitten by that (CLAUDE.md's
// vacuous-oracle rule). The one-shot spellings appear only in the ORDER rows,
// which assert the printed output, not the leak.
//
// ⚠ THE VALUES ARE PINNED, NOT JUST THE EXIT CODES. A process exit status is
// masked to 0..255 and cannot carry a string value, so every row that can
// assert a value does so through the captured stdout.
//
// ⚠ THE CONTROLS ARE THE POINT. A fix that disposed a BOUND value would
// double-free it (the ASan leg catches that as a hard error), and a fix that
// disposed nothing would leave the leak rows red. Both directions are pinned.

#include <cstdlib>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/force_sanitize.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

// Save/restore BEZEL_AOT_DETECT_LEAKS around a row (the #4638 knob). Required,
// not tidiness: the gate may run a whole shard with the knob set. Restoring
// means the rows are order- and ambient-independent either way.
class LeakEnv {
public:
    explicit LeakEnv(const char* value) {
        const char* prev = std::getenv("BEZEL_AOT_DETECT_LEAKS");
        had_ = prev != nullptr;
        if (had_) prev_ = prev;
        if (value) ::setenv("BEZEL_AOT_DETECT_LEAKS", value, /*overwrite=*/1);
        else       ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    ~LeakEnv() {
        if (had_) ::setenv("BEZEL_AOT_DETECT_LEAKS", prev_.c_str(), 1);
        else      ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    LeakEnv(const LeakEnv&) = delete;
    LeakEnv& operator= (const LeakEnv&) = delete;

private:
    bool had_ = false;
    std::string prev_;
};

// The class the defect is about: a USER class (not a runtime-managed type —
// §28.9's tier is exhaustive and names no user class, so a `new Cls()` IS an
// owned value), with a destructor so the ORDER of the disposal is observable.
//
// A MACRO, not a `const char*` constant: the programs below must stay
// `constexpr` (so a compile error in a program is a compile error in this
// file, and the program text is visible in the row that uses it), and a
// `constexpr` string cannot be built by concatenating a `std::string`.
#define BEZEL_CLS_DECL                                                        \
    "class Cls "                                                               \
    "  public constructor() end constructor; "                                  \
    "  public override method ToString(): string => \"cls\"; "                   \
    "  destructor Console.WriteLine(\"disposed\"); end destructor; "            \
    "end Cls; "

// ── THE REPRO PROGRAMS ─────────────────────────────────────────────────────

// #7793: `Console.WriteLine(new Cls())` — the console's `Object` parameter is
// plain (stdlib §2.1), so the fresh class temporary is a borrowing use.
constexpr const char* kConsoleNewLoop =
    "module M; " BEZEL_CLS_DECL
    "class C public static method Main(): int "
    "  for var i in 0..<200 do "
    "    Console.WriteLine(new Cls()); "
    "  end for; "
    "  return 0; "
    "end Main; end C;";

// #7793: the `Console.Out` spelling — the same built-in console, reached
// through the `TextWriter` value (stdlib §2.1 / §7.4).
constexpr const char* kConsoleOutNewLoop =
    "module M; " BEZEL_CLS_DECL
    "class C public static method Main(): int "
    "  for var i in 0..<200 do "
    "    Console.Out.WriteLine(new Cls()); "
    "  end for; "
    "  return 0; "
    "end Main; end C;";

// #7855: `Guid.FromByteArray(g.ToByteArray())` — `ToByteArray` is declared
// `owned Array<byte>` (stdlib §2.6) and `FromByteArray` borrows its parameter
// (stdlib §2.6: it COPIES the 16 bytes out).
constexpr const char* kGuidFromBytesLoop =
    "module M; class C public static method Main(): int "
    "  var n := 0; "
    "  for var i in 0..<200 do "
    "    var g := Guid.NewGuid(); "
    "    if Guid.FromByteArray(g.ToByteArray()).Equals(g) then n := n + 1; end if; "
    "  end for; "
    "  Console.WriteLine(n); "
    "  return 0; "
    "end Main; end C;";

// #7855: the `Encoding.UTF8.GetBytes(s)` twin (stdlib §3.2's
// `owned Array<byte>`) — the other producer the issue names.
//
// The two GUIDs are built from the SAME bytes, so the comparison is TRUE by
// construction (the value is not compared against a random GUID, whose bytes
// have nothing to do with the ASCII text).
constexpr const char* kGuidGetBytesLoop =
    "module M; class C public static method Main(): int "
    "  var n := 0; "
    "  for var i in 0..<200 do "
    "    var g1 := Guid.FromByteArray(Encoding.UTF8.GetBytes(\"0123456789abcdef\")); "
    "    var g2 := Guid.FromByteArray(Encoding.UTF8.GetBytes(\"0123456789abcdef\")); "
    "    if g1.Equals(g2) then n := n + 1; end if; "
    "  end for; "
    "  Console.WriteLine(n); "
    "  return 0; "
    "end Main; end C;";

// ── THE CONTROLS (must stay CLEAN) ─────────────────────────────────────────

// CONTROL 1: the NAMED-LOCAL arm of the console shape — the local's own
// scope-end discharge owns the value, so the fix must NOT touch it (a second
// disposal would be a double free, caught by the ASan leg).
constexpr const char* kConsoleNamedLoop =
    "module M; " BEZEL_CLS_DECL
    "class C public static method Main(): int "
    "  for var i in 0..<200 do "
    "    var c := new Cls(); "
    "    Console.WriteLine(c); "
    "  end for; "
    "  return 0; "
    "end Main; end C;";

// CONTROL 2: the `using var` arm of the #7855 shape — the BINDING takes the
// obligation, so the call site must not dispose the borrowed argument a
// second time.
constexpr const char* kGuidUsingVarLoop =
    "module M; class C public static method Main(): int "
    "  var n := 0; "
    "  for var i in 0..<200 do "
    "    var g := Guid.NewGuid(); "
    "    using var u := g.ToByteArray(); "
    "    if Guid.FromByteArray(u).Equals(g) then n := n + 1; end if; "
    "  end for; "
    "  Console.WriteLine(n); "
    "  return 0; "
    "end Main; end C;";

// CONTROL 3: the `return` arm — a value transferred into a `return` is held
// by the return slot (§28.3.11), so it must NOT be disposed by the call.
constexpr const char* kGuidReturnLoop =
    "module M; class C "
    "  public static method Make(g: Guid): owned Array<byte> "
    "    return g.ToByteArray(); "
    "  end Make; "
    "  public static method Main(): int "
    "  var n := 0; "
    "  for var i in 0..<200 do "
    "    var g := Guid.NewGuid(); "
    "    var b := C.Make(g); "
    "    n := n + b.Count; "
    "  end for; "
    "  Console.WriteLine(n); "
    "  return 0; "
    "end Main; end C;";

// CONTROL 4: the NESTED arm — the temporary is the argument of an INNER call
// that is itself an argument of an outer one. A fix that only handled the
// outermost argument would leave this one leaking.
constexpr const char* kGuidNestedLoop =
    "module M; class C "
    "  public static method Take(a: Array<byte>): int return a.Count; end Take; "
    "  public static method Outer(n: int): int return n + 1; end Outer; "
    "  public static method Main(): int "
    "  var n := 0; "
    "  for var i in 0..<200 do "
    "    var g := Guid.NewGuid(); "
    "    n := n + C.Outer(C.Take(g.ToByteArray())); "
    "  end for; "
    "  Console.WriteLine(n); "
    "  return 0; "
    "end Main; end C;";

// ── THE #7803 PROGRAMS ─────────────────────────────────────────────────────
//
// §28.9: a function value is RUNTIME-MANAGED — its closure cell is a heap
// object with a reference count, and the +1 a producer mints on it is the
// caller's to release. §28.3.11 (spec line 7712) governs the UNBOUND case:
// a fresh owned temporary "used only by *borrowing* operations within the
// expression — as the receiver of a further call, as a non-`owned` argument"
// "is disposed at the **end of the full expression** in which it is produced".
// §28.3 (spec line 7759) makes the console's own parameter a BORROWING USE:
// `Console.WriteLine(value: Object)` (stdlib §2.1) takes its parameter PLAIN,
// so the console never takes the +1 and the discharge is the call site's.
//
// #7803: `Console.WriteLine(MkFn())` — the closure cell is minted by a CALL
// (the #429 RETURN category: the callee "mints exactly +1 on return"), and
// nothing binds it. MEASURED on this tree's parent (`bezelc --sanitize=
// address`, LSan): 200 × 64 B leaked in 200 allocations — the `bz.closure`
// cell is {ptr, ptr, i64, ptr, i64, i8, ptr, ptr} = 64 bytes.
constexpr const char* kConsoleFreshClosureLoop =
    "module M; class C "
    "  public static method MkFn(): (int) -> int => lambda(x: int) => x + 1; "
    "  public static method Main(): int "
    "    for var i in 0..<200 do "
    "      Console.WriteLine(MkFn()); "
    "    end for; "
    "    return 0; "
    "  end Main; end C;";

// #7803: the `Console.Out` spelling — the same built-in console reached
// through the `TextWriter` value (stdlib §2.1 / §7.4).
constexpr const char* kConsoleOutFreshClosureLoop =
    "module M; class C "
    "  public static method MkFn(): (int) -> int => lambda(x: int) => x + 1; "
    "  public static method Main(): int "
    "    for var i in 0..<200 do "
    "      Console.Out.WriteLine(MkFn()); "
    "    end for; "
    "    return 0; "
    "  end Main; end C;";

// #7803: the LAMBDA-CALLEE twin — the closure is minted by a call to a
// LAMBDA-typed local (`isLambdaClosureReturnCall`), whose body shape is
// statically known to mint the +1.
constexpr const char* kConsoleFreshClosureLambdaCalleeLoop =
    "module M; class C public static method Main(): int "
    "  var mk: () -> (int) -> int := lambda() => lambda(x: int) => x + 1; "
    "  for var i in 0..<200 do "
    "    Console.WriteLine(mk()); "
    "  end for; "
    "  return 0; "
    "end Main; end C;";

// ── THE #7803 CONTROLS (must stay CLEAN) ────────────────────────────────────

// CONTROL 5: the INLINE-LAMBDA arm — a lambda literal is a +1 transient the
// call site owns, and the console already released it (#5943). This row pins
// that the new discharge did not turn that release into a second one (a
// double free, which the ASan leg reports as a hard error).
constexpr const char* kConsoleInlineLambdaLoop =
    "module M; class C public static method Main(): int "
    "  for var i in 0..<200 do "
    "    Console.WriteLine(lambda(x: int) => x + 1); "
    "  end for; "
    "  return 0; "
    "end Main; end C;";

// CONTROL 6: a function-valued FIELD — the owner is the object, not the
// console, so the console must not discharge it. (A function value stored in
// a field must be capture-free (§28.14); the lambda below captures nothing.)
constexpr const char* kConsoleFnFieldLoop =
    "module M; class Box public F: (int) -> int; "
    "  public constructor() F := lambda(x: int) => x + 1; end constructor; "
    "end Box; "
    "class C public static method Main(): int "
    "  var b := new Box(); "
    "  for var i in 0..<200 do "
    "    Console.WriteLine(b.F); "
    "  end for; "
    "  return 0; "
    "end Main; end C;";

// CONTROL 7: a function-valued PARAMETER — a plain function-typed parameter
// is call-bounded (§28.14): the callee borrows it, so the console must not
// discharge it either.
constexpr const char* kConsoleFnParamLoop =
    "module M; class C "
    "  public static method Use(f: (int) -> int): int "
    "    for var i in 0..<200 do "
    "      Console.WriteLine(f); "
    "    end for; "
    "    return 0; "
    "  end Use; "
    "  public static method Main(): int "
    "    return C.Use(lambda(x: int) => x + 1); "
    "  end Main; end C;";

// CONTROL 8: the NAMED-LOCAL arm of the #7803 shape — the local owns the
// producer's +1 and releases it at its scope end, so the console must NOT
// discharge it (a double free).
//
// ⚠ THE LEAK ORACLE IS OFF FOR THIS ROW, DELIBERATELY, and the reason is a
// defect OUT OF THIS LANE'S SCOPE. A closure local passed to the CONSOLE
// leaks its 64-byte cell today: the closure-escape walk that decides whether
// a function-typed local may release its cell at scope end
// (`ClosureEscapeWalker`, src/codegen/LlvmBackend_Setup.cpp — a RESERVED
// file for this lane) classifies EVERY call argument as an escape, and its
// only relief (`closureArgBindsMethodOwnTypeParamBorrow`) needs a resolved
// user MethodSymbol — which the BUILT-IN `Console.WriteLine` does not have.
// The walk therefore withholds the local's scope-end `bz_closure_release`
// and the cell leaks.
//
// ⇒ WHAT THIS ROW PINS IS THE DIRECTION THAT MATTERS HERE: the console must
// NOT discharge a BOUND closure. With the leak oracle off, a double free is
// still a hard ASan error (the row fails), so the row is NOT vacuous — it is
// exactly the "do not dispose a bound value" guard the fix must respect.
// The leak itself is reported in the lane's handback, with the file and line
// that owns it.
constexpr const char* kConsoleFnNamedLocalLoop =
    "module M; class C public static method Main(): int "
    "  var f: (int) -> int := lambda(x: int) => x + 1; "
    "  for var i in 0..<200 do "
    "    Console.WriteLine(f); "
    "  end for; "
    "  return 0; "
    "end Main; end C;";

// ── THE ORDER PROGRAMS (one shot — the OUTPUT is the assertion) ────────────

// The disposal runs AFTER the borrowing use (§28.3.11's "Disposal runs *after*
// the borrowing use, so the receiver is live for the duration of the call"):
// the console renders the instance through its `ToString` FIRST, and the
// destructor runs at the end of the full expression — i.e. after the write.
constexpr const char* kConsoleOrder =
    "module M; " BEZEL_CLS_DECL
    "class C public static method Main(): int "
    "  Console.WriteLine(new Cls()); "
    "  return 0; "
    "end Main; end C;";

std::string repeat(const char* s, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

}  // namespace

// ══ THE REPRO ROWS ═════════════════════════════════════════════════════════

// #7793: `Console.WriteLine(new Cls())` — the fresh class temporary is
// disposed at the end of the full expression. Pre-fix this row fails with
// 200 × 16 B leaked.
BEZEL_TEST(codegen_console_guid_call_arg_temp_7793_7855,
           console_write_line_fresh_class_temp_is_disposed_28_3_11) {
    LeakEnv env("strict");
    auto r = runAotChecked(kConsoleNewLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, repeat("cls\ndisposed\n", 200));
}

// #7793: the `Console.Out` spelling — the same built-in console reached
// through the `TextWriter` value.
BEZEL_TEST(codegen_console_guid_call_arg_temp_7793_7855,
           console_out_write_line_fresh_class_temp_is_disposed_28_3_11) {
    LeakEnv env("strict");
    auto r = runAotChecked(kConsoleOutNewLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, repeat("cls\ndisposed\n", 200));
}

// #7855: `Guid.FromByteArray(g.ToByteArray())` — the fresh owned array is
// disposed at the end of the full expression. Pre-fix this row fails with
// 200 × 96 B leaked.
BEZEL_TEST(codegen_console_guid_call_arg_temp_7793_7855,
           guid_from_byte_array_fresh_owned_array_temp_is_disposed_28_3_11) {
    LeakEnv env("strict");
    auto r = runAotChecked(kGuidFromBytesLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
}

// #7855: the `Encoding.UTF8.GetBytes(s)` twin — the other producer the issue
// names.
BEZEL_TEST(codegen_console_guid_call_arg_temp_7793_7855,
           guid_from_byte_array_fresh_getbytes_temp_is_disposed_28_3_11) {
    LeakEnv env("strict");
    auto r = runAotChecked(kGuidGetBytesLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
}

// ══ THE ORDER ROW ══════════════════════════════════════════════════════════

// §28.3.11: "Disposal runs *after* the borrowing use, so the receiver is live
// for the duration of the call." The console renders the instance through its
// `ToString` (so "cls" is printed) and the destructor runs at the END of the
// full expression (so "disposed" is printed AFTER it).
//
// Pre-fix the destructor never ran at all — the instance was owned by nobody
// — so this row failed on the missing "disposed" line AND on the leak.
BEZEL_TEST(codegen_console_guid_call_arg_temp_7793_7855,
           console_write_line_disposes_after_the_write_28_3_11) {
    LeakEnv env("strict");
    auto r = runAotChecked(kConsoleOrder);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "cls\ndisposed\n");
}

// ══ THE CONTROLS ══════════════════════════════════════════════════════════

// CONTROL 1: the NAMED-LOCAL arm — the local's own scope-end discharge owns
// the value. The fix must not dispose it a second time (a double free, which
// the ASan leg reports as a hard error).
BEZEL_TEST(codegen_console_guid_call_arg_temp_7793_7855,
           control_named_local_class_temp_is_not_double_disposed) {
    LeakEnv env("strict");
    auto r = runAotChecked(kConsoleNamedLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, repeat("cls\ndisposed\n", 200));
}

// CONTROL 2: the `using var` arm — the binding takes the obligation, so the
// borrowed argument must not be disposed by the call site.
BEZEL_TEST(codegen_console_guid_call_arg_temp_7793_7855,
           control_using_var_bound_array_is_not_double_disposed) {
    LeakEnv env("strict");
    auto r = runAotChecked(kGuidUsingVarLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
}

// CONTROL 3: the `return` arm — a value transferred into a `return` is held
// by the return slot; it must not be disposed twice.
BEZEL_TEST(codegen_console_guid_call_arg_temp_7793_7855,
           control_returned_owned_array_is_not_double_disposed) {
    LeakEnv env("strict");
    auto r = runAotChecked(kGuidReturnLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "3200\n");
}

// CONTROL 4: the NESTED arm — the temporary is the argument of an INNER call
// that is itself an argument of an outer one.
BEZEL_TEST(codegen_console_guid_call_arg_temp_7793_7855,
           control_nested_call_fresh_owned_array_temp_is_disposed) {
    LeakEnv env("strict");
    auto r = runAotChecked(kGuidNestedLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "3400\n");
}

// ══ THE #7803 ROWS ═════════════════════════════════════════════════════════

// #7803: `Console.WriteLine(MkFn())` — the closure cell minted by the call is
// an unbound owned temporary (§28.3.11): the console's `Object` parameter is
// plain (stdlib §2.1), so the console never takes the +1, and the call site
// releases it at the end of the full expression. Pre-fix this row fails with
// 200 × 64 B leaked (the `bz.closure` cell).
BEZEL_TEST(codegen_console_guid_call_arg_temp_7793_7855,
           console_write_line_fresh_closure_temp_is_released_28_3_11) {
    LeakEnv env("strict");
    auto r = runAotChecked(kConsoleFreshClosureLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, repeat("(int) -> int\n", 200));
}

// #7803: the `Console.Out` spelling — the same built-in console reached
// through the `TextWriter` value.
BEZEL_TEST(codegen_console_guid_call_arg_temp_7793_7855,
           console_out_write_line_fresh_closure_temp_is_released_28_3_11) {
    LeakEnv env("strict");
    auto r = runAotChecked(kConsoleOutFreshClosureLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, repeat("(int) -> int\n", 200));
}

// #7803: the LAMBDA-CALLEE twin — the closure is minted by a call to a
// lambda-typed local, whose body shape is statically known to mint the +1.
BEZEL_TEST(codegen_console_guid_call_arg_temp_7793_7855,
           console_write_line_fresh_closure_lambda_callee_is_released) {
    LeakEnv env("strict");
    auto r = runAotChecked(kConsoleFreshClosureLambdaCalleeLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, repeat("(int) -> int\n", 200));
}

// ══ THE #7803 CONTROLS ═════════════════════════════════════════════════════

// CONTROL 5: the INLINE-LAMBDA arm — a lambda literal is a +1 transient the
// call site owns and the console already releases it (#5943). The new
// discharge must not become a second release of the same cell (a double
// free).
BEZEL_TEST(codegen_console_guid_call_arg_temp_7793_7855,
           control_inline_lambda_closure_is_not_double_released) {
    LeakEnv env("strict");
    auto r = runAotChecked(kConsoleInlineLambdaLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, repeat("(int) -> int\n", 200));
}

// CONTROL 6: a function-valued FIELD — the owner is the object, so the
// console must not discharge it.
BEZEL_TEST(codegen_console_guid_call_arg_temp_7793_7855,
           control_field_owned_closure_is_not_released_by_the_console) {
    LeakEnv env("strict");
    auto r = runAotChecked(kConsoleFnFieldLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, repeat("(int) -> int\n", 200));
}

// CONTROL 7: a function-valued PARAMETER — a plain function-typed parameter is
// call-bounded (§28.14), so the console must not discharge it.
BEZEL_TEST(codegen_console_guid_call_arg_temp_7793_7855,
           control_param_owned_closure_is_not_released_by_the_console) {
    LeakEnv env("strict");
    auto r = runAotChecked(kConsoleFnParamLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, repeat("(int) -> int\n", 200));
}

// CONTROL 8: the NAMED-LOCAL arm — the local owns the producer's +1, so the
// console must NOT discharge it.
//
// The LEAK ORACLE IS OFF HERE, deliberately, and the reason is a defect OUT OF
// THIS LANE'S SCOPE: the closure-escape walk
// (`ClosureEscapeWalker`, src/codegen/LlvmBackend_Setup.cpp — RESERVED for
// this lane) classifies every call ARGUMENT as an escape, and its only relief
// (`closureArgBindsMethodOwnTypeParamBorrow`) needs a resolved user
// MethodSymbol, which the BUILT-IN `Console.WriteLine` does not have — so the
// local's scope-end `bz_closure_release` is withheld and the 64-byte cell
// leaks. `runAotCheckedLenient` is the harness's own helper for exactly that
// situation ("a test whose program intentionally or incidentally leaks"): the
// generated-code AddressSanitizer stays ACTIVE, so a double free is still a
// hard error and this row is NOT vacuous — it pins "the console must not
// dispose a bound closure", which is the direction the fix must respect. The
// leak itself is reported in the lane's handback, with the file and line that
// owns it.
BEZEL_TEST(codegen_console_guid_call_arg_temp_7793_7855,
           control_named_local_closure_is_not_released_by_the_console) {
    LeakEnv env(nullptr);  // the 64-byte residual leak is out of scope; see above
    auto r = runAotCheckedLenient(kConsoleFnNamedLocalLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, repeat("(int) -> int\n", 200));
}
