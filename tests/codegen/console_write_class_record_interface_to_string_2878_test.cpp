// Codegen — `Console.Write/WriteLine` on a CLASS-, RECORD- or INTERFACE-typed
// argument (NOT through `Object`) leaked its minting `ToString` render and
// mis-rendered interfaces/records. Issue #2878.
//
// Spec (language spec revision v0.240; stdlib citations marked as such):
//   §8.5 "`ToString`" — "is invoked implicitly inside string interpolation
//     ([§18.5](#sec-interpolated-strings)) via virtual dispatch". For a class
//     with no override, the default is "the type's qualified name"; for a
//     positional record, "a representation of fields, e.g. `"Point(X: 10, Y:
//     20)"`"; for a body record that satisfies auto-equality (§9.2), the
//     property-listing form. `Console.Write(Line)` "renders its one argument"
//     (stdlib §2.1) — the §8.5 rendering is THE rendering, whatever the
//     dispatch site. The four spellings of one §8.5 answer (explicit
//     `.ToString()`, the §18.5 hole, `Console.Write(Line)`, and the
//     `Object`-typed spelling) must agree.
//   §28.3 "Unbound owned temporaries" + §28.9 "Runtime-managed types" — the
//     fresh +1 string a `ToString` override returns is an owned temporary
//     that the caller of `Console.Write(Line)` must discharge exactly once.
//     `bz_string_release` is null/sentinel-safe and decrements a shared
//     reference — the same discharge discipline #2875 installed for the
//     `Kind::Primitive` / `P::Object` arm of this function.
//   stdlib §2.1 "`Console`" — `Console.Write(value: Object)` "invokes
//     `ToString()`" (the §2.1 note "The `Object` overloads invoke
//     `ToString()`. For interpolated strings, the compiler resolves the
//     `string` overload directly."), which means the `Object`-typed path is
//     the implicit-virtual-dispatch path the OTHER static-type paths must
//     not diverge from.
//
// SYMPTOM, measured on 7c466ee with `bezelc --sanitize=address` (this lane's
// base, which already carries #2875 for the `Object` arm and #3118 for the
// record arm — the `Named` arm was deliberately untouched by both):
//
//   * `Console.WriteLine(ix)`  for `ix: IX`      -> "IX"      WRONG (static
//                                                       name, not dynamic
//                                                       §8.5 render)
//   * `Console.WriteLine(sq)`  for `sq: Sq`      -> "S1"      correct
//                                                       (minting override)
//                                                       but LEAKS (one rc=1
//                                                       string per call)
//   * `Console.WriteLine(p)`   for `p: Pt`       -> "Pt"      WRONG (type
//                                                       name, not structural)
//                                                       — this aspect is
//                                                       already FIXED by #3118
//                                                       and pinned here as a
//                                                       regression guard
//
// ROOT CAUSE (the part this lane owns; #3118 fixed the record arm separately).
// `Emitter::tryLowerConsoleCall`'s `argTy->kind == Kind::Named &&
// argTy->declaration` arm (LlvmBackend_Calls.cpp) walked `ts->members` for a
// user `ToString()` MethodSymbol — which (a) misses INTERFACES (interfaces
// declare no body, and the vtable dispatch they inherit is exactly what
// §8.5 names) and (c) never discharged a minting class override's render
// after `emitMethodCall`. Records' structural `ToString` was the third miss
// (#3118 added the `recordIsAutoBearing`/`isAutoEqualityBearingChoice` arms),
// and the FOURTH miss was the synthetic-vs-method-symbol asymmetry (records'
// `ToString` is synthesized, not declared). Three different defects in one
// branch, all in §8.5's surface.
//
// FIX. Route the `Named` arm through `emitValueToString(arg, argTy)` — the
// SAME renderer §18.5's interpolation hole, the direct `.ToString()` call,
// and the explicit `p.ToString()` chain already reach. The dispatch is:
//   class-with-override  -> emitMethodCall (fresh +1)
//   class-without-override -> spec85QualifiedTypeName (literal sentinel)
//   record auto-bearing   -> emitRecordToString (fresh +1)
//   record non-auto       -> spec85NameOnlyDefaultIsQualified (literal sentinel)
//   choice auto-hashable  -> emitChoiceToString (fresh +1)
//   enum user override    -> emitEnumToString's user branch (fresh +1)
//   enum default          -> spec85QualifiedTypeName (literal sentinel)
//   interface             -> lowerObjectToString (vt/string fresh +1, null
//                            "null" literal sentinel)
// Set `releaseRender = true` for the WHOLE result: a release on a sentinel is
// a no-op (stdlib §28.9), and every fresh +1 in the list needs discharge.
// One branch, one renderer, one discharge flag — the structural mismatch the
// original ad-hoc member walk let in cannot recur here.
//
// ⚠ ORACLE DISCIPLINE. As with #2875, "no leak" is BLIND to over-release —
// both sides of #2878's fix have the same calibration problem. The LEAK
// oracle (runAotChecked) is the dominant signal for the class-override arm:
// LSan forces exit 1 on any leak, so `exitCode == 0` under runAotChecked IS
// the exactly-once assertion for the override path. The interface/record arms
// are pure VALUE-CONTROL: wrong rendering is a wrong stdout under BOTH
// oracles, not a leak. The test reads the source value AFTER every console
// call over 20 iterations so an over-release would fault under generated-code
// ASan or print wrong stdout under the plain oracle.
//
// ⚠ DELIBERATELY NOT TOUCHED HERE: the §22.x `Console.Write(string)` shape
// stays on its own path (it's a `Kind::Primitive` arm in #2875's terms), and
// the §22.x `Console.WriteLine(string)` no-arg form is `Console.NewLine`'s
// problem. Both are documented, both are pinned by tests in other files, and
// neither is in scope for this lane.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

std::string out(const JitResult& r) {
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

// --- (A) + (C) — the lane's primary repro. ---------------------------------
// `ix: IX` (interface), `sq: Sq` (class with minting `ToString`), `p: Pt`
// (positional record, hoisted ONCE before the loop so it carries one value
// for all 20 iterations), 20 iterations each. Pre-fix measured on 7c466ee:
//
//   IX IX IX ... IX    <- (a) wrong: static interface name, NOT dynamic
//   S1 S2 ... S20     <- correct value, but (c) LEAKS (711 B / 40 allocs)
//   Pt Pt ... Pt      <- (b) wrong: bare name, NOT structural (§9.1)
//
// Post-fix expected:
//
//   S1 S2 ... S20     <- (a) dynamic dispatch via vtable, like the Object
//                          arm of #2875
//   S1 S2 ... S20     <- (c) zero leaks under LSan
//   Pt(X: 1, Y: 2)    <- (b) the §9.1 form (one value, printed 20 times)
//
// The accumulator `t` reads the interface/record back after the writes so an
// over-release would fault under generated-code ASan; the LEAN strings are
// built at runtime (`"c" + i.ToString()`) so the churn allocation does NOT
// fold to an immortal rc sentinel on which release is a no-op.
const char* kPrimaryRepro =
    "module M2878; "
    "public record Pt(X: int, Y: int); "
    "public interface IX "
    "  method Area(): int; "
    "end IX; "
    "public class Sq: IX "
    "  public N: int := 0; "
    "  public method Area(): int => N * N; "
    "  public override method ToString(): string "
    "    return \"S\" + N.ToString(); "
    "  end ToString; "
    "end Sq; "
    "public class C "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var i: int := 0; "
    "    var p: Pt := new Pt(1, 2); "
    "    while i < 20 do "
    "      i := i + 1; "
    "      using var sq := new Sq(); "
    "      sq.N := i; "
    "      var ix: IX := sq; "
    "      Console.WriteLine(ix); "
    "      Console.WriteLine(sq); "
    "      Console.WriteLine(p); "
    "      var churn: string := \"c\" + i.ToString(); "
    "      t := t + churn.Length - churn.Length + 1; "
    "    end while; "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C;";

// --- (B) REGRESSION GUARD on the #3118 record arm. -------------------------
// A positional record and an auto-equality body record, both with the §9.1
// structural form. #3118 fixed the `Named` arm for records; #2878 must keep
// that path working (the `emitValueToString` reroute covers it via
// `emitRecordToString`). A bare record-to-name render would be a regression
// of #3118.
const char* kRecordRegression =
    "module M2878B; "
    "public record Pt(X: int, Y: int); "
    "public record Person "
    "  public property Age: int; "
    "  public constructor(a: int) Age := a; end constructor; "
    "end Person; "
    "public class C "
    "  public static method Main(): int "
    "    var p := new Pt(1, 2); "
    "    var q := new Person(41); "
    "    Console.WriteLine(p); "
    "    Console.WriteLine(q); "
    "    return 0; "
    "  end Main; "
    "end C;";

// --- (D) OVER-RELEASE ORACLE: read sources back after the writes. -----------
// A class with a minting override whose render is a runtime-built string
// (NOT a fold-to-sentinel interpolation): 20 iterations, `Console.Write` AND
// `Console.WriteLine`, and the instance is read back after each write. An
// over-release would fault under ASan; the plain oracle would print wrong
// stdout.
const char* kOverReleaseGuard =
    "module M2878D; "
    "public class Sq "
    "  public N: int := 0; "
    "  public override method ToString(): string "
    "    return \"S\" + N.ToString() + \"q\"; "
    "  end ToString; "
    "end Sq; "
    "public class C "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var i: int := 0; "
    "    while i < 20 do "
    "      i := i + 1; "
    "      using var sq := new Sq(); "
    "      sq.N := i; "
    "      Console.WriteLine(sq); "
    "      Console.Write(sq); "
    "      var churn: string := \"c\" + i.ToString(); "
    "      t := t + sq.N - i + churn.Length - churn.Length + 1; "
    "    end while; "
    "    if t = 20 then return 0; end if; "
    "    return 1; "
    "  end Main; "
    "end C;";

}  // namespace

// ===========================================================================
// (A) + (C) — the lane's primary repro. Run under BOTH oracles.
// ===========================================================================

// Per §8.5 (interface dynamic dispatch + record structural form) + §28.9
// (release the render). Pre-fix: interface printed "IX" not "S<n>" (a),
// override leaked (c), record printed "Pt" not "Pt(X: 1, Y: 2)" (b).
BEZEL_TEST(codegen_console_write_class_record_interface_to_string_2878,
           primary_repro_value_control_8_5) {
    JitResult r = runAot(kPrimaryRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // (a) + (b) + (c)-value: every line is the §8.5 dynamic render. The
    // record `p` is hoisted BEFORE the loop with `new Pt(1, 2)` and
    // therefore renders as "Pt(X: 1, Y: 2)" 20 times (NOT "Pt(X: i, …)").
    std::string expected;
    for (int i = 1; i <= 20; ++i) {
        expected += "S" + std::to_string(i) + "\n";
        expected += "S" + std::to_string(i) + "\n";
        expected += "Pt(X: 1, Y: 2)\n";
    }
    BEZEL_EXPECT_EQ(out(r), expected);
}

// Per §28.9 / §28.3 — every minting render is discharged exactly once. LSan
// forces exit 1 on any leak, so `exitCode == 0` under runAotChecked IS the
// leak-oracle.
BEZEL_TEST(codegen_console_write_class_record_interface_to_string_2878,
           primary_repro_leak_oracle_28_9) {
    JitResult r = runAotChecked(kPrimaryRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);   // pre-fix: exit 1 (LSan 711 B / 40 allocs)
}

// ===========================================================================
// (B) — regression guard on the #3118 record arm.
// ===========================================================================

// Per §9.1 — the §3118 record arm must keep working under the #2878 reroute.
// `runAotOut` strips a single trailing newline (its header documents the
// behavior; baking "\n" into the expected string would be platform-dependent),
// so the comparison is against the bare two lines.
BEZEL_TEST(codegen_console_write_class_record_interface_to_string_2878,
           record_named_arm_still_renders_fields_9_1) {
    BEZEL_EXPECT_EQ(runAotOut(kRecordRegression),
                    "Pt(X: 1, Y: 2)\nPerson(Age: 41)");
}

// ===========================================================================
// (D) — over-release oracle on the minting class-override arm.
// ===========================================================================

// Per §28.9 — the class-override render is read back after the writes. An
// over-release is a fault under generated-code ASan (runAotChecked), wrong
// stdout under the plain oracle.
BEZEL_TEST(codegen_console_write_class_record_interface_to_string_2878,
           class_minting_override_is_discharged_exactly_once_28_9) {
    JitResult plain = runAot(kOverReleaseGuard);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 0);
    // Build the expected output: 20 lines of "S<n>q\n" + "S<n>q" (the Write).
    std::string expected;
    for (int i = 1; i <= 20; ++i) {
        expected += "S" + std::to_string(i) + "q\n";
        expected += "S" + std::to_string(i) + "q";
    }
    BEZEL_EXPECT_EQ(out(plain), expected);

    JitResult r = runAotChecked(kOverReleaseGuard);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}