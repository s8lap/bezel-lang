// fix #2498 (§9.2/§9.4/§23, v0.236): a LOCAL (non-imported) record's DIRECT
// computed property setter (`Emitter::lowerPropertySetValue`,
// LlvmBackend_Assign.cpp) emitted MALFORMED IR and crashed the backend.
//
// Root cause: the receiver-resolution line
//     recv = ma->target ? lowerExpr(*ma->target) : currentThis_;
// bound whatever `lowerExpr` produced directly as `currentThis_` with NO
// pointer/aggregate check at all. A record receiver lowers to a BY-VALUE
// struct (issue #347 / §9.2), so binding that struct value as `this` made
// the inlined setter body's implicit field write (`_n := value;`, resolved
// via `lvalueAddr`/`implicitFieldAddr`, which always expects a POINTER
// `this`) emit a `StructGEP` against a non-pointer base. Confirmed on
// origin/main by direct probe (this file's own T1 repro) — the verifier
// rejected the emitted module:
//   "GEP base pointer is not a vector or a vector of pointers"
//   "Store operand must be a pointer."
//   bezel codegen: IR verification failed (malformed IR emitted; ...)
// — an outright compile-time abort, not a runtime issue.
//
// This is the LOCAL sibling of #2489's `lowerImportedPropertySetValue` fix
// (same file, f4caed1): that function had the identical missing-spill gap
// for an IMPORTED record's setter, plus a second, independent trap — naively
// spilling a LOADED COPY of the receiver (the pattern every sibling READ
// site in this codebase uses) is wrong for a SETTER, because `lowerExpr` on
// a record-typed local LOADS a byte-for-byte copy; writing through a spilled
// copy mutates a throwaway alloca, never the caller's actual variable. Both
// issues are fixed here the same way #2489 fixed its own function: prefer
// `lvalueAddr` (the receiver's OWN storage address, resolved via
// `localSlot`/`fieldAddr`/etc.) over `lowerExpr` (a copy); fall back to
// `lowerExpr` + spill only when the receiver expression is genuinely NOT an
// addressable place (e.g. reached through a property-returned CLASS
// reference, where no field/local storage backs it directly but the value
// IS already a pointer to the real shared object — T4 below). The spill is
// guarded to `isAggregateType() && !isEnumReceiver`, mirroring #2489's own
// enum-exclusion rationale: a pre-existing, separate sema gap (#2499, NOT
// fixed here) currently lets a block-form settable property through on an
// ENUM in violation of §10.5, and an enum receiver's scalar `i32` must never
// be treated as a struct `this` pointer.
//
// T1/T2: the crash repro (public/private backing) — fails to even COMPILE
// pre-fix (IR verification failure), compiles/links/runs and returns the
// correct WRITTEN value post-fix. T3: a local CLASS control (unaffected,
// guards against a regression from the shared receiver-resolution rewrite).
// T4: a receiver reached through a property chain (a CLASS-typed property
// result — genuinely non-addressable via `lvalueAddr`'s structural helpers,
// but a valid pointer once evaluated — exercises the `lowerExpr`-fallback
// pointer branch without a spill). T5: the enum defensive-guard regression
// mirroring #2489's own T12, pinning that the wrongly-accepted (#2499)
// enum setter shape stays link-safe (no crash), not "fixed" by this commit.
//
// CONSTANTS kept < 256 (runAot's exit code is WEXITSTATUS-masked to 8 bits,
// per the lane protocol) and chosen so no two operands could coincidentally
// produce the same sum, ruling out a "looks right" false pass.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// T1 — CORE repro: public backing field, positional + body member combined
// (§9.2/§9.3). Direct (non-interface) SET then READ. Pre-fix: IR
// verification failure (crash, does not even link). Post-fix: 9 + 55 = 64.
BEZEL_TEST(property_setter_local_record_2498, public_backing_direct_set_then_read) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record RecSetLocal2498T1(x: int) "
        "  public _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end RecSetLocal2498T1; "
        "public class C2498T1 "
        "  public static method Main(): int "
        "    var r := new RecSetLocal2498T1(9); "
        "    r.Name := 55; "
        "    return r.x + r.Name; "
        "  end Main; "
        "end C2498T1;"), 64);
}

// T2 — private BACKING field axis: the setter mutates a PRIVATE field
// through the inlined accessor body. x=9, set Name:=47 -> 9 + 47 = 56.
BEZEL_TEST(property_setter_local_record_2498, private_backing_direct_set_then_read) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record RecSetLocal2498T2(x: int) "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end RecSetLocal2498T2; "
        "public class C2498T2 "
        "  public static method Main(): int "
        "    var r := new RecSetLocal2498T2(9); "
        "    r.Name := 47; "
        "    return r.x + r.Name; "
        "  end Main; "
        "end C2498T2;"), 56);
}

// T3 — CONTROL: a local CLASS's direct settable property, set-then-read.
// Classes were never affected by the record by-value-receiver bug (a class
// reference always lowers to a pointer), but this guards against a
// regression from rewriting the shared receiver-resolution code.
BEZEL_TEST(property_setter_local_record_2498, class_control_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class ClsSetLocal2498T3 "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end ClsSetLocal2498T3; "
        "public class C2498T3 "
        "  public static method Main(): int "
        "    using var c := new ClsSetLocal2498T3(); "
        "    c.Name := 77; "
        "    return c.Name; "
        "  end Main; "
        "end C2498T3;"), 77);
}

// T4 — receiver reached through a PROPERTY chain, not a plain local: `h`'s
// `LeafProp` getter returns a CLASS-typed (reference) value, so the chain
// `h.LeafProp.Name := 88` sets a property whose OWN receiver expression
// (`h.LeafProp`) is not addressable via `lvalueAddr`'s field/local helpers
// (it is a computed property read, not a field) — this exercises the
// `lowerExpr` fallback branch, which must recognize the already-pointer-
// shaped result and use it directly (no spill). Because `Leaf` is a
// reference type, the write genuinely persists back to the shared object
// (unlike a value-typed/record chain, which §23.5's sema check rejects
// outright as "modifies a discarded copy" before this ever reaches
// codegen — confirmed by probe, not exercised here).
BEZEL_TEST(property_setter_local_record_2498, property_chain_class_receiver_fallback) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Leaf2498T4 "
        "  public _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end Leaf2498T4; "
        "public class Holder2498T4 "
        "  private owned _leaf: Leaf2498T4 := new Leaf2498T4(); "
        "  public property LeafProp: Leaf2498T4 "
        "    get => _leaf; "
        "    set => _leaf := value; "
        "  end LeafProp; "
        "end Holder2498T4; "
        "public class C2498T4 "
        "  public static method Main(): int "
        "    using var h := new Holder2498T4(); "
        "    h.LeafProp.Name := 88; "
        "    return h.LeafProp.Name; "
        "  end Main; "
        "end C2498T4;"), 88);
}

// T5 — §10.5 (v0.236): a block-form property on an ENUM is now REJECTED.
//
// HISTORY, because this test's meaning inverted mid-batch and that is worth
// recording: as originally written it was a link-safety guard. #2499 was then
// an open sema gap that wrongly ACCEPTED this shape (§10.5 forbids "Block-form
// properties or properties with backing storage" on an enum), so the program
// compiled, and the guard pinned that this fix's `isEnumReceiver` check kept
// the resulting inert setter path link-safe rather than crashing.
//
// #2499 landed three commits later in the same batch, from a different lane,
// and closed that gap — so the premise this test rested on ("the sema gap
// stays open") became false while the test still asserted `r >= 0`, and it
// began failing. Caught by independent review of the two commits together,
// before the gate ran.
//
// Repurposed rather than deleted: the shape is still worth pinning, just with
// the opposite expectation. It now asserts the §10.5 REJECTION, which also
// keeps a guard on #2499's own behaviour from this file's side. The
// `isEnumReceiver` guard in this fix remains correct and still matters for any
// enum receiver that reaches codegen by another route; it is simply no longer
// observable through this particular program.
BEZEL_TEST(property_setter_local_record_2498, enum_block_form_property_rejected_10_5_2499) {
    // Assert on diagnostic TEXT, not the coarse "did not run": runAot's
    // WIFSIGNALED path synthesizes an Error for a runtime SIGSEGV too, so a
    // bare `!r.ran` check cannot tell a genuine rejection from a crash.
    // Same idiom as enum_property_restrictions_2499_test.cpp's own hasError().
    auto r = bezel::test::runAot(
        "module M; "
        "public enum EnumSetLocal2498T5 "
        "  Red, Green, Blue; "
        "  public property P: int "
        "    get => 7; "
        "    set => do end; "
        "  end P; "
        "end EnumSetLocal2498T5; "
        "public class C2498T5 "
        "  public static method Main(): int "
        "    var e := EnumSetLocal2498T5.Red; "
        "    e.P := 42; "
        "    return e.P; "
        "  end Main; "
        "end C2498T5;");
    bool sawEnumBlockFormDiag = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find("block-form property") != std::string::npos) {
            sawEnumBlockFormDiag = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(sawEnumBlockFormDiag);
}

// T6 — BONUS coverage: `lowerPropertyCompound` (§22.9, `r.Name += v`) reads
// via the getter (already spill-safe post-#2474) then WRITES via
// `lowerPropertySetValue` — the exact function this commit fixes. Confirms
// the fix's benefit extends to compound assignment on a local record
// property, not only plain `:=`, with no separate code change needed.
// _n starts 10; `r.Name += 3` reads 10, computes 13, writes 13 back.
// Pre-fix: this crashed identically to T1 (same malformed-IR shape),
// confirmed by probe before this file was written.
BEZEL_TEST(property_setter_local_record_2498, compound_assign_delegates_to_fixed_setter) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public record RecCompoundProp2498T6(seed: int) "
        "  public _n: int := 10; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end RecCompoundProp2498T6; "
        "public class C2498T6 "
        "  public static method Main(): int "
        "    var r := new RecCompoundProp2498T6(1); "
        "    r.Name += 3; "
        "    return r.Name; "
        "  end Main; "
        "end C2498T6;"), 13);
}
