// Spec-derived regression test for issue #2543 — the SYMMETRIC reject path to
// #2530: a bare READ of a write-only (set-only block) INDEXER is rejected.
// #2530 closed the ref/out operand gap (ExprTypecheck.cpp's
// checkRefOutPropertyOperand + the user-defined-indexer visit's
// `setExprSymbol` attachment); #2543 closes the bare-read gap — the indexer
// analogue of #1192 / #2461's read-of-write-only NAMED-PROPERTY gate
// (`propertyDeclIsSetOnly` in ExprTypecheck_Calls.cpp), which deliberately
// excludes indexers because IndexExpr is a distinct AST node from
// MemberAccessExpr and reaches its own visit path.
//
// §23 (v0.236): "Reading a write-only property is an error (it has no getter
// to call)." The named-property case already fires this diagnostic; the
// INDEXER case did NOT — `var x := d[0]` where `d`'s indexer is set-only
// previously fell through to codegen and silently degraded to a zero-
// initialized value via the #2434 fallback. This test pins the rejection.
//
// NOTE on `rejects()` (per tests/README.md and #2417's precedent): `runAot`'s
// WIFSIGNALED branch synthesizes an Error diagnostic for a runtime SIGSEGV
// too, so `rejects()` is NOT by itself the fail-without/pass-with pin. The
// `hasSetOnlyIndexerReadError` helper (asserts the specific diagnostic TEXT
// emitted by the fix) is the actual discriminator: pre-fix the program
// compiles clean and produces zero (the silent-no-op defect), so the
// diagnostic never fires; post-fix the diagnostic fires with the §23 cite.
// ACCEPT controls assert `ran && exitCode == expected` to catch a false
// reject of the legal write/read-write shapes.
//
// Mechanical verification:
//   - reverting just src/sema/ExprTypecheck_Exprs.cpp's #2543 block leaves
//     all REJECT cases returning zero (the silent-no-op defect); all the
//     ACCEPT controls continue to compile + run.
//   - re-applying the block flips every REJECT case's diagnostic-text
//     assertion to passing; ACCEPT controls are unaffected.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bool runs(int expected, const std::string& src) {
    auto r = runAot(src);
    return r.ran && r.exitCode == expected;
}

bool rejects(const std::string& src) {
    auto r = runAot(src);
    return !r.ran && r.diagnostics.hasErrors();
}

// True iff an ERROR diagnostic carries #2543's write-only indexer read text.
// Matches the SAME substring the named-property #1192 gate emits, so a
// user greps "write-only" and lands both — the new INDEXER case reuses the
// exact phrasing shape ("X is write-only (it declares only a `set`
// accessor) and cannot be read").
bool hasSetOnlyIndexerReadError(const std::string& src) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("write-only") == std::string::npos) continue;
        if (d.message.find("set") == std::string::npos) continue;
        if (d.message.find("cannot be read") == std::string::npos) continue;
        if (d.message.find("23") == std::string::npos) continue;
        return true;
    }
    return false;
}

// ---- REJECT case 1 (the issue's own repro, local block-form set-only) -----
const char* kLocalSetOnlyIndexerBareReadRejected =
    "module M; "
    "class C "
    "  public _x: int := 0; "
    "  public property [i: int]: int set => do _x := value; end; end; "
    "  public static method Main(): int "
    "    using var c := new C(); "
    "    var x := c[0]; "  // bare READ of a write-only indexer
    "    return x; "
    "  end Main; "
    "end C;";

// ---- REJECT case 2 (compound `+=` reads first → still a bare read) -------
const char* kLocalSetOnlyIndexerCompoundAssignRejected =
    "module M; "
    "class C "
    "  public _x: int := 0; "
    "  public property [i: int]: int set => do _x := value; end; end; "
    "  public static method Main(): int "
    "    using var c := new C(); "
    "    c[0] += 3; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---- REJECT case 3 (block-form set-only with arrow-body setter arm —
//                    alternate syntax, same write-only semantics as case 1)
//                    (Note: the parser's PropertyBody grammar only admits
//                    `=> Expression` as the entire property body — there is
//                    no expression-bodied SET-only form. A single-arm
//                    `set => ...; end;` is BLOCK-bodied with one accessor.)
const char* kLocalSetOnlyIndexerArrowBlockRejected =
    "module M; "
    "class C "
    "  public _x: int := 0; "
    "  public property [i: int]: int set => _x := value; end; "
    "  public static method Main(): int "
    "    using var c := new C(); "
    "    return c[0]; "
    "  end Main; "
    "end C;";

// ---- REJECT case 4 (indexer read used as a method argument — read in
//                    another read position; not the := LHS, not a write) --
const char* kLocalSetOnlyIndexerReadAsArgRejected =
    "module M; "
    "class C "
    "  public _x: int := 0; "
    "  public property [i: int]: int set => do _x := value; end; end; "
    "  public static method Main(): int "
    "    using var c := new C(); "
    "    var x := c[0] + 1; "
    "    return x; "
    "  end Main; "
    "end C;";

// ---- ACCEPT control 1 (plain `:=` write to set-only indexer — the LEGAL
//      escape hatch, exactly like `c.Val := 7` for #1192's named property;
//      the write dispatches to the setter, NOT a read). -------------------
const char* kLocalSetOnlyIndexerPlainWriteAccepted =
    "module M; "
    "class C "
    "  public _x: int := 0; "
    "  public property [i: int]: int set => do _x := value; end; end; "
    "  public method Show(): int => _x; "
    "  public static method Main(): int "
    "    using var c := new C(); "
    "    c[0] := 7; "
    "    return c.Show(); "  // 7 — confirms the write actually ran
    "  end Main; "
    "end C;";

// ---- ACCEPT control 2 (read-WRITE indexer read — getter present, so the
//      read is legal and the value round-trips). ---------------------------
const char* kLocalReadWriteIndexerReadAccepted =
    "module M; "
    "class C "
    "  public _x: int := 0; "
    "  public property [i: int]: int get => _x; set => do _x := value; end; end; "
    "  public static method Main(): int "
    "    using var c := new C(); "
    "    c[0] := 7; "
    "    return c[0]; "  // 7 — read-write round-trip
    "  end Main; "
    "end C;";

// ---- ACCEPT control 3 (expression-bodied READ-only indexer — auto-derived
//      getter, write-only exemption does not apply). ----------------------
const char* kLocalReadOnlyIndexerReadAccepted =
    "module M; "
    "class C "
    "  public _x: int := 42; "
    "  public property [i: int]: int => _x + i; "  // getter-only
    "  public static method Main(): int "
    "    using var c := new C(); "
    "    return c[5]; "  // 47
    "  end Main; "
    "end C;";

// ---- ACCEPT control 4 (set-only indexer via interface, READ ONLY —
//      analogous to #1192's named-property ACCEPT: the GETTER-ONLY
//      interface indexer satisfies via a setter-less impl, which has
//      the getter to call; the set-only local impl never enters the
//      picture because the call dispatches through the iface itable,
//      which exposes the getter side of the implementing class — i.e.
//      any read-write interface indexer is a legal read. This pins
//      against an over-eager rejection of an iface-typed read that
//      coincidentally targets a class whose only indexer is set-only.
//      [Note: this exact shape is rejected by #2417's iface-accessor-set
//      check when the iface declares a read-write indexer and the impl
//      is missing the setter; but here the impl IS read-write, just
//      structured as separate get/set arms on the same property.]) ----
//
//      To keep this control simple and within #2543's exact scope, we
//      skip the iface shape and use a direct local read-write impl
//      already covered by kLocalReadWriteIndexerReadAccepted above. The
//      IMPORTED case is covered by the module-level test in
//      xmod_setonly_indexer_read_rejected_2543_test.cpp.
}  // namespace

// ---- FAIL-without / PASS-with: bare READ of a set-only LOCAL indexer -----

BEZEL_TEST(setonly_indexer_read_rejected_2543,
           local_block_setonly_indexer_bare_read_rejected_23) {
    BEZEL_EXPECT_TRUE(rejects(kLocalSetOnlyIndexerBareReadRejected));
    BEZEL_EXPECT_TRUE(
        hasSetOnlyIndexerReadError(kLocalSetOnlyIndexerBareReadRejected));
}

BEZEL_TEST(setonly_indexer_read_rejected_2543,
           local_setonly_indexer_compound_assign_rejected_23) {
    BEZEL_EXPECT_TRUE(
        rejects(kLocalSetOnlyIndexerCompoundAssignRejected));
    BEZEL_EXPECT_TRUE(hasSetOnlyIndexerReadError(
        kLocalSetOnlyIndexerCompoundAssignRejected));
}

BEZEL_TEST(setonly_indexer_read_rejected_2543,
           local_setonly_indexer_arrow_block_rejected_23) {
    BEZEL_EXPECT_TRUE(
        rejects(kLocalSetOnlyIndexerArrowBlockRejected));
    BEZEL_EXPECT_TRUE(hasSetOnlyIndexerReadError(
        kLocalSetOnlyIndexerArrowBlockRejected));
}

BEZEL_TEST(setonly_indexer_read_rejected_2543,
           local_setonly_indexer_read_as_arg_rejected_23) {
    BEZEL_EXPECT_TRUE(rejects(kLocalSetOnlyIndexerReadAsArgRejected));
    BEZEL_EXPECT_TRUE(
        hasSetOnlyIndexerReadError(kLocalSetOnlyIndexerReadAsArgRejected));
}

// ---- ACCEPT controls (over-rejection safety — these must NOT regress) -----

BEZEL_TEST(setonly_indexer_read_rejected_2543,
           local_setonly_indexer_plain_write_accepted_runs_correct_value) {
    BEZEL_EXPECT_TRUE(
        runs(7, kLocalSetOnlyIndexerPlainWriteAccepted));
}

BEZEL_TEST(setonly_indexer_read_rejected_2543,
           local_readwrite_indexer_read_accepted_runs_correct_value) {
    BEZEL_EXPECT_TRUE(runs(7, kLocalReadWriteIndexerReadAccepted));
}

BEZEL_TEST(setonly_indexer_read_rejected_2543,
           local_readonly_indexer_read_accepted_runs_correct_value) {
    BEZEL_EXPECT_TRUE(runs(47, kLocalReadOnlyIndexerReadAccepted));
}