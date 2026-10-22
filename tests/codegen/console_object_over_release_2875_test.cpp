// Codegen — `Console.Write` / `Console.WriteLine` on an `Object`-typed argument
// OVER-RELEASED its rendered string: the post-write reclaim was a raw `free()`
// on a value that is not always a private malloc. Issue #2875.
//
// Spec (language spec revision v0.240; stdlib citations are marked as such):
//   §8.5 "`ToString`" — "The compiler-generated default returns: - For classes:
//     the type's qualified name." That default lowers to an immortal string
//     LITERAL, which is the value `free()` aborted on.
//   stdlib §9.6 "`String`" — "public pure override method ToString(): string;
//     // returns this". The render of an `Object` holding a string therefore IS
//     the caller's own live string (retained since #2846), and `free()`ing it
//     destroyed the caller's data.
//   §28.9 "Runtime-managed types" — lists "`string` — pervasive; created by
//     literals, interpolation, concatenation, and library methods" among the
//     runtime-managed types, and leaves the mechanism open: "The implementation
//     of runtime-managed types' lifecycle (reference counting, region
//     allocation, or other) is not part of this specification." The mechanism
//     Bezel's implementation chose is reference counting, and a shared
//     reference is discharged by DECREMENTING it, never by handing its storage
//     to free().
//   §28.1 "Where the `owned` modifier can appear" — "the parameter takes
//     ownership of the value at the call site" is what an `owned` parameter
//     does; stdlib §2.1 declares "public static method WriteLine(value:
//     Object);" with NO `owned`, so the console never acquires the argument and
//     has nothing of the caller's to destroy.
//   §8.6 "Boxing" — "When a value type is used where `Object` (or an interface
//     implemented by the value type) is expected, the value is **boxed**: a
//     heap-allocated wrapper holds the value. Boxed values are runtime-managed
//     ([§28.9])." Two boxed kinds render to a sentinel (`bool` → stdlib
//     §9.4's "True"/"False"; an `enum` with §8.5's default → the type's
//     qualified name), the same immortal-literal hazard down a third path.
//
// SYMPTOM, measured on 7c466ee (this lane's base, which already carries #2865
// and #2846) with `bezelc --sanitize=address`. `tryLowerConsoleCall` classified
// its argument with `isFreeablePrim(p) = p != String && p != Bool` and, when
// true, reclaimed the render with `free(data); free(obj)`. `PrimitiveType`'s
// `Object` member is neither of the two names that predicate excludes, so every
// `Object`-typed console argument took the raw free — while its render comes
// from `emitPrimitiveToString(_, P::Object)` → `lowerObjectToString`, whose
// three arms hand back an immortal literal, the RECEIVER itself, or a fresh
// mint. Only the third survives a free(). Measured, one call each:
//
//     var oc: Object := kk;   Console.WriteLine(oc)   class → "free(): invalid
//                                pointer", exit 134 (SEGV in the allocator
//                                under ASan: a WRITE to a .rodata address)
//     var os: Object := src;  Console.WriteLine(os)   live string → src is
//                                DESTROYED, silently; the next read is
//                                "SEGV ... READ ... in bz_string_to_utf8z"
//     var ol: Object := "lit";                        immortal literal → abort
//     var ob: Object := true;                         box → "True" sentinel →
//                                                     abort
//     var oe: Object := Color.Green;                  box → type-name sentinel
//                                                     → abort
//     var on: Object? := null;                        → "null" literal → abort
//     Console.Write(oc)                               same, both facets
//   CLEAN before and after (the arms that genuinely mint): a boxed int / double
//   / char / decimal / record, and a class or enum with a MINTING `ToString`
//   override.
//
// THE FIX (LlvmBackend_Calls.cpp): discharge the render with the RC-AWARE
// `bz_string_release` and key that on the property "the console converted the
// argument" (`strPtr != arg`) instead of on an enumeration of primitive kinds.
// `bz_string_release` is a no-op on a sentinel or null, a decrement on a shared
// reference, and exactly the `free(data); free(obj)` pair the old code emitted
// on a private rc==1 render — so it is correct for all three arms at once, and
// the predicate can no longer be wrong about a kind nobody thought of. The
// `P::String` argument never reaches the conversion, so the BORROWED string
// argument is still left to the #1031 producer arm.
//
// ⚠ ORACLE DISCIPLINE. A "no leak" assertion is BLIND to this bug — an
// over-release leaks nothing. Every case below re-reads the source value AFTER
// the console call, with a runtime-built churn allocation in between so a freed
// block is likely reused, over 20 iterations; the class cases read the instance
// back and the string cases read the string's contents back and print them, so
// a destroyed value shows up as a fault (under generated-code ASan) or as wrong
// stdout (under the plain oracle) rather than as silence. Strings are built at
// RUNTIME (`"ab" + "cd".Substring(0, 2)`) — folding two literals yields an
// immortal rc sentinel on which release is a no-op, which would make the probe
// vacuous. The opposite direction (exactly-once, no leak traded for the
// over-release) has its own leak-asserted tests over the whole boxable-kind
// grid.
//
// ⚠ DELIBERATELY NOT FIXED HERE, and filed instead: the sibling `Named` branch
// of the same function — a CLASS-, RECORD- or INTERFACE-typed console argument
// (no `Object` in the spelling) — renders through its own ad-hoc member walk
// and discharges nothing. Measured on the same build: a minting `ToString`
// override leaks its render once per call (711 B / 40 allocations over 20
// iterations), an interface-typed argument prints the STATIC interface name
// instead of the dynamic §8.5 rendering, and a record prints its type name
// instead of its structural form. That is the BOUNDED direction plus two
// rendering defects; both are out of this fix's scope.
//
// ⚠ ONE SHAPE TRADES AN ACCIDENTAL RECLAIM FOR A BOUNDED LEAK, deliberately and
// measured: `Console.WriteLine(h.M())` where `M(): Object` MINTS a fresh string
// (`=> "x" + S`). The render retains the receiver and this fix releases it once,
// which is balanced — but the un-bound temporary's OWN `+1` has no owner, so it
// now leaks 38 B/call (760 B / 40 allocations over 20 iterations). The raw
// free() used to destroy that string outright, which happened to be leak-free
// for this one shape and was heap corruption for every other one. It is also
// exactly what the INTERPOLATION sibling `$"[{h.M()}]"` already does on a path
// this fix does not touch — measured at the identical 760 B / 40 allocations —
// so the two positions are now consistent rather than one of them being novel.
// Giving that temporary an owner needs callee-side knowledge of whether an
// `Object`-returning method transfers a `+1` string, which #2865 declined to
// guess at for the box spelling for the same reason. Filed, not guessed.

// #6778 (owner ruling, §5.1, §22.6, v0.340): an un-narrowed `Object?` is not
// `Object`, so the null arm (`on`, `o12`) prints through §18.5's
// interpolation hole, which renders the same `null`. The old spelling is a
// reject row in tests/codegen/console_nullable_arg_6778_test.cpp.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

std::string out(const JitResult& r) {
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

// --- Shape A: a CLASS instance through `Object`. ---------------------------
// §8.5's default rendering for a class is "the type's qualified name", an
// immortal literal — pre-fix `free()`d, which aborted the process inside the
// allocator. `kk.v` is read after BOTH console facets so the instance itself is
// also proven un-freed.
const char* kClassThroughObject =
    "module M2875A; "
    "public class K2875A "
    "  public v: int; "
    "  public constructor(n: int) v := n; end constructor; "
    "end K2875A; "
    "public class C2875A "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      using var kk := new K2875A(k + 1); "
    "      var oc: Object := kk; "
    "      Console.WriteLine(oc); "
    "      Console.Write(oc); "
    "      Console.WriteLine(); "
    "      var churn: string := \"c\" + k.ToString(); "
    "      t := t + kk.v - k + churn.Length - churn.Length; "
    "      k := k + 1; "
    "    end while; "
    "    if t = 20 then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end C2875A;";

// --- Shape B: a live STRING through `Object`. ------------------------------
// stdlib §9.6 — `ToString` on a string "returns this", so the render IS `src`.
// Pre-fix
// `free()` destroyed it: the first line printed, the remaining 20 did not, and
// under ASan the second read faulted in `bz_string_to_utf8z`. `src.Length` is
// read every iteration AND the contents are printed 21 times, so a destroyed
// string cannot pass either oracle.
const char* kStringThroughObject =
    "module M2875B; "
    "public class C2875B "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    var src: string := \"ab\" + \"cd\".Substring(0, 2); "
    "    while k < 20 do "
    "      var os: Object := src; "
    "      Console.WriteLine(os); "
    "      var churn: string := \"c\" + k.ToString(); "
    "      t := t + src.Length - 3 + churn.Length - churn.Length; "
    "      k := k + 1; "
    "    end while; "
    "    Console.WriteLine(src); "
    "    if t = 20 then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end C2875B;";

// --- The other three IMMORTAL-render arms. ---------------------------------
// A string LITERAL in an Object slot (the receiver is itself the sentinel), a
// boxed `bool` (stdlib's "True"/"False" globals), a boxed `enum` with §8.5's
// default rendering (the type's name), and `Object?` holding null (the "null"
// literal `lowerObjectToString` emits). All four aborted pre-fix; none of them
// allocates, so LSan is on and must stay clean.
const char* kSentinelRenderArms =
    "module M2875C; "
    "public enum Color2875C Red, Green; end Color2875C; "
    "public class C2875C "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      var ol: Object := \"lit\"; "
    "      var ob: Object := true; "
    "      var oe: Object := Color2875C.Green; "
    "      var on: Object? := null; "
    "      Console.WriteLine(ol); "
    "      Console.WriteLine(ob); "
    "      Console.WriteLine(oe); "
    // #6778 (§5.1, §22.6, v0.340): an un-narrowed `Object?` is not `Object`;
    // the null arm prints through §18.5's hole, which renders `null`.
    "      Console.WriteLine($\"{on}\"); "
    "      var churn: string := \"c\" + k.ToString(); "
    "      t := t + churn.Length - churn.Length + 1; "
    "      k := k + 1; "
    "    end while; "
    "    if t = 20 then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end C2875C;";

// --- The EXACTLY-ONCE direction: every MINTING arm, leak-asserted. ---------
// These are the arms that were already correct under the raw free() — the fix
// must not trade the over-release for a leak. A boxed int / double / char /
// decimal / record / enum and a class with a minting `ToString` override, each
// written through `Object` 20 times, plus the directly-typed primitives and a
// `$"..."` argument (the #1031 producer arm, which must keep working).
const char* kMintingArmsReleasedOnce =
    "module M2875D; "
    "public record Pt2875D(X: int, Y: int); "
    "public enum Color2875D Red, Green; end Color2875D; "
    "public class KMint2875D "
    "  public N: int := 0; "
    "  public override method ToString(): string "
    "    return \"K\" + N.ToString(); "
    "  end ToString; "
    "end KMint2875D; "
    "public class C2875D "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    while k < 20 do "
    "      k := k + 1; "
    "      var oi: Object := k; "
    "      var od: Object := 1.5; "
    "      var oc: Object := 'z'; "
    "      var om: Object := 2.5m; "
    "      var orr: Object := new Pt2875D(k, k); "
    "      var oe: Object := Color2875D.Green; "
    "      Console.WriteLine(oi); "
    "      Console.WriteLine(od); "
    "      Console.WriteLine(oc); "
    "      Console.WriteLine(om); "
    "      Console.WriteLine(orr); "
    "      Console.WriteLine(oe); "
    "      using var km := new KMint2875D(); "
    "      km.N := k; "
    "      var ok: Object := km; "
    "      Console.WriteLine(ok); "
    "      Console.WriteLine(k); "
    "      Console.WriteLine(1.5); "
    "      Console.WriteLine('z'); "
    "      Console.WriteLine(2.5m); "
    "      Console.WriteLine(true); "
    "      Console.WriteLine($\"[{k}]\"); "
    "      var churn: string := \"c\" + k.ToString(); "
    "      t := t + churn.Length - churn.Length + 1; "
    "    end while; "
    "    if t = 20 then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end C2875D;";

// --- OVER-RELEASE ORACLE over the remaining axes. --------------------------
// A `ToString` override that returns a stored FIELD (so the value the console
// discharges is one the object still holds — the callee's return-side mint is
// the only thing that makes the release balanced); a TEMPORARY argument (an
// `Object`-returning method) next to a BOUND local; `Console.Write` as well as
// `WriteLine`; the interpolation sibling position on the same local; and a
// plain `string` local as the accept-control. Every source value is read back
// and PRINTED after the 20 rounds, so an over-release is a fault under ASan and
// wrong stdout under the plain oracle.
//
// ⚠ LENIENT (ASan on, leak detection OFF) is the right oracle here, and for a
// measured reason: `var os: Object := src;` binds a string local into an Object
// local, which StringEscapeWalker treats as an escape, so `src`'s own +1 is
// deliberately never released — the residual #2846 documented and #2865's
// `string_through_object_untouched` guard also runs lenient for. Measured on
// this lane's base WITHOUT any console call on `os` at all: the identical
// "35 byte(s) leaked in 2 allocation(s)". It is not this fix's.
const char* kOverReleaseAxes =
    "module M2875E; "
    "public class Rc2875E "
    "  public Name: string := \"r\"; "
    "  public override method ToString(): string "
    "    return Name; "
    "  end ToString; "
    "end Rc2875E; "
    "public class H2875E "
    "  public S: string := \"h\"; "
    "  public method M(): Object => S; "
    "end H2875E; "
    "public class C2875E "
    "  public static method Main(): int "
    "    var t: int := 0; "
    "    var k: int := 0; "
    "    using var rc := new Rc2875E(); "
    "    rc.Name := \"n\" + \"ame\".Substring(0, 3); "
    "    using var h := new H2875E(); "
    "    h.S := \"s\" + \"elf\".Substring(0, 3); "
    "    var src: string := \"ab\" + \"cd\".Substring(0, 2); "
    "    var plain: string := \"pq\" + \"rs\".Substring(0, 2); "
    "    while k < 20 do "
    "      var orc: Object := rc; "
    "      Console.WriteLine(orc); "              // vtable arm, field return
    "      var os: Object := src; "
    "      Console.WriteLine(os); "               // string arm, bound local
    "      Console.Write(os); "
    "      Console.WriteLine(); "
    "      var rendered: string := $\"[{os}]\"; " // the sibling position
    "      Console.WriteLine(plain); "            // accept-control: a borrow
    "      Console.WriteLine(h.M()); "            // string arm, TEMPORARY
    "      var churn: string := \"c\" + k.ToString(); "
    "      t := t + rendered.Length - 5 + churn.Length - churn.Length; "
    "      k := k + 1; "
    "    end while; "
    "    Console.WriteLine(rc.Name); "
    "    Console.WriteLine(src); "
    "    Console.WriteLine(plain); "
    "    Console.WriteLine(h.S); "
    "    if t = 20 then return 41; end; "
    "    return 1; "
    "  end Main; "
    "end C2875E;";

// --- VALUE CONTROL: the fix is refcount bookkeeping and must not change a ---
// single rendered character. §8.5's per-kind table, written through `Object`.
const char* kRenderedTextUnchanged =
    "module M2875F; "
    "public record Pt2875F(X: int, Y: int); "
    "public enum Color2875F Red, Green; end Color2875F; "
    "public class KDef2875F public N: int := 0; end KDef2875F; "
    "public class KMint2875F "
    "  public override method ToString(): string "
    "    return \"K\" + \"9\".Substring(0, 1); "
    "  end ToString; "
    "end KMint2875F; "
    "public class C2875F "
    "  public static method Main(): int "
    "    using var kd := new KDef2875F(); "
    "    using var km := new KMint2875F(); "
    "    var s: string := \"ab\" + \"cd\".Substring(0, 2); "
    "    var o1: Object := new Pt2875F(1, 2); "
    "    var o2: Object := 7; "
    "    var o3: Object := Color2875F.Green; "
    "    var o4: Object := kd; "
    "    var o5: Object := km; "
    "    var o6: Object := s; "
    "    var o7: Object := true; "
    "    var o8: Object := 'z'; "
    "    var o9: Object := 2.5m; "
    "    var o10: Object := 1.5; "
    "    var o11: Object := \"lit\"; "
    "    var o12: Object? := null; "
    "    Console.WriteLine(o1); Console.WriteLine(o2); Console.WriteLine(o3); "
    "    Console.WriteLine(o4); Console.WriteLine(o5); Console.WriteLine(o6); "
    "    Console.WriteLine(o7); Console.WriteLine(o8); Console.WriteLine(o9); "
    "    Console.WriteLine(o10); Console.WriteLine(o11); "
    // #6778 (§5.1, §22.6, v0.340): `o12` is an un-narrowed `Object?`; it
    // prints through §18.5's hole, which renders the same `null`.
    "    Console.WriteLine($\"{o12}\"); "
    "    return 0; "
    "  end Main; "
    "end C2875F;";

}  // namespace

// ===========================================================================
// (A) The two shapes the issue names.
// ===========================================================================

// per §8.5 ("for classes: the type's qualified name") + §28.9 — the render is
// an immortal literal; free()ing it aborted the process.
BEZEL_TEST(codegen_console_object_over_release_2875,
           class_through_object_console_does_not_free_the_render_8_5) {
    JitResult plain = runAot(kClassThroughObject);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);   // pre-fix: 134 (free(): invalid pointer)
    JitResult r = runAotChecked(kClassThroughObject);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// per stdlib §9.6 (`String.ToString` "returns this") + §28.9 — the render IS the
// caller's live string; free()ing it destroyed the caller's data silently.
BEZEL_TEST(codegen_console_object_over_release_2875,
           string_through_object_console_does_not_destroy_it_9_6) {
    JitResult plain = runAot(kStringThroughObject);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    std::string expected;
    for (int i = 0; i < 21; ++i) expected += "abcd\n";
    BEZEL_EXPECT_TRUE(out(plain) == expected);   // pre-fix: one line, then empties
    JitResult r = runAotCheckedLenient(kStringThroughObject);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);   // pre-fix: SEGV in bz_string_to_utf8z
}

// per §8.5 / §8.6 — the other three immortal-render arms (a literal receiver, a
// boxed `bool`, a boxed `enum` with the default rendering) and the null arm.
BEZEL_TEST(codegen_console_object_over_release_2875,
           sentinel_render_arms_are_not_freed_8_6) {
    JitResult plain = runAot(kSentinelRenderArms);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);   // pre-fix: 134
    JitResult r = runAotChecked(kSentinelRenderArms);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// ===========================================================================
// (B) The opposite direction — the fix must not trade an over-release for a
// leak. LSan forces exit 1 on any leak, so `exitCode == 41` under
// `runAotChecked` IS the exactly-once assertion.
// ===========================================================================

// per §8.6 + §28.9 — every MINTING arm of the same lowering, 20 rounds each.
BEZEL_TEST(codegen_console_object_over_release_2875,
           minting_render_arms_are_released_exactly_once_28_9) {
    JitResult plain = runAot(kMintingArmsReleasedOnce);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    JitResult r = runAotChecked(kMintingArmsReleasedOnce);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// ===========================================================================
// (C) The remaining axes of the over-release direction.
// ===========================================================================

// per §28.1 (only an `owned` parameter takes ownership at the call site; stdlib
// §2.1's `WriteLine(value: Object)` is not one) + stdlib §9.6 + §18.5 —
// a field-returning `ToString`, a temporary vs a bound local, `Write` vs
// `WriteLine`, the interpolation sibling, and a plain `string` accept-control.
BEZEL_TEST(codegen_console_object_over_release_2875,
           field_return_temporary_and_borrow_axes_are_not_freed_28_1) {
    JitResult plain = runAot(kOverReleaseAxes);
    BEZEL_EXPECT_TRUE(plain.ran);
    BEZEL_EXPECT_EQ(plain.exitCode, 41);
    BEZEL_EXPECT_TRUE(out(plain).find("\nname\nabcd\npqrs\nself\n") !=
                      std::string::npos);
    JitResult r = runAotCheckedLenient(kOverReleaseAxes);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// ===========================================================================
// (D) Value control — not one rendered character may change.
// ===========================================================================

// per §8.5's per-kind rendering table, read through `Object`.
BEZEL_TEST(codegen_console_object_over_release_2875,
           rendered_text_is_unchanged_8_5) {
    JitResult r = runAot(kRenderedTextUnchanged);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(out(r) ==
        // #2934 (§8.5, v0.252): the ENUM row ("Color2875F") and the
        // no-override CLASS row ("KDef2875F") are now module-path-
        // qualified; the record head stays unqualified, and the class
        // WITH an override ("K9") is untouched.
        "Pt2875F(X: 1, Y: 2)\n7\nM2875F.Color2875F\nM2875F.KDef2875F\nK9\nabcd\n"
        "True\nz\n2.5\n1.5\nlit\nnull\n");
}
