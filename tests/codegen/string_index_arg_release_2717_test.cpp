// Codegen / refcounting — issue #2717: a `string` local or `+1` producer
// TEMPORARY passed as an INDEX ARGUMENT to a user-defined indexer accessor is
// never released.
//
// Spec: §23.5 "Restrictions" — "`ref` and `out` parameters are not allowed in
// indexer parameter lists"; §28.1 "Where the `owned` modifier can appear" — on
// "a primitive, `string`, a function type, an ordinary record, an enum, a
// non-owning inline array, or the unit type — `owned` is a compile error: such
// values carry no disposal obligation to transfer (§28.9)". Together those fix
// an indexer index argument as ALWAYS an unmodified by-value BORROW whose one
// reference belongs to the caller — structurally the ordinary argument-pass
// position of §23.4 "Indexers". Also §28.9 "Runtime-managed types", §4.6
// "`string`", §6.6 "Compound assignments", §17.6 "The increment and decrement
// operators", and §22.3 "Parameter modifiers". Spec revision v0.240.
//
// ⚠ A CITATION TRAP THIS FILE EXISTS TO NOT REPEAT. §23.5's rule governs the
// indexer's OWN parameter list — it does NOT say anything about an argument
// whose *location is* an indexer slot. That shape is admitted by a different
// rule, §22.3 "`ref` and `out` argument locations": "A `ref` or `out` argument
// must be an assignable storage location — ... a local variable, a writable
// field, an **indexer slot**, or a property with an accessible setter."
// Reading §23.5 as if it excluded `ref t[k]` is exactly how case (L) below —
// a reachable DOUBLE FREE plus use-after-free — was very nearly shipped.
//
// THE DEFECT, in two disjoint halves that the same call site produced:
//
//   (1) a NAMED LOCAL (`var s := $"v-{i}"; t[s] := i;`). StringEscapeWalker's
//       IndexExpr arm walked every index with `safeRead=false`
//       (LlvmBackend_Setup.cpp), so the local was tainted ESCAPED, its
//       scope-end `bz_string_release` never armed, and its +1 leaked once per
//       access. The fix is the INDEXER-ACCESS form of #1430's call-bounded-
//       borrow relaxation in walkCallArgs — the same extension #2023 made for
//       `new T(args)` and #2535/#2540 made for the property/indexer SET value.
//
//   (2) a `+1` PRODUCER TEMPORARY (`t[$"v-{i}"] := i;`). Bound to no local, so
//       no scope-end release could ever cover it, and no call site released it
//       either: #429's ARGUMENT-PASS release (LlvmBackend_Calls2.cpp ~7834)
//       covers an ordinary call's arguments but was never extended to an
//       indexer's index arguments.
//
// The two halves are DISJOINT by construction: (2)'s gate is
// `isPlusOneStringProducer`, which never matches an IdentifierExpr, so it
// cannot release a caller-owned local; (1) only arms the owner's own scope-end
// release. Cases (J) and (K) below are the guards for that separation.
//
// ⚠ UNMASKED BY #2712, NOT INDEPENDENT OF IT. Pre-#2712 the index temporary was
// freed by the very unpaired `bz_string_release` that #2712 paired with a
// retain — the same release that, for a caller-owned local, was the #2712
// use-after-free. #2712 converted that accidental (and in the named-local case
// fatal) free into this bounded leak; this file closes it properly.
//
// ⚠ Every string is built at RUNTIME (`$"v-{i}"`): `"ab" + "cd"` constant-folds
// to an rc sentinel on which `bz_string_release` no-ops — the trap that has
// made probes in this family vacuous repeatedly.
//
// MECHANICAL FAIL -> PASS on 2a990de, both oracles:
//   * ASan/LSan : `731 byte(s) leaked in 40 allocation(s)` (640 B of 20
//     bz_string headers + 91 B of payloads) -> clean;
//   * plain (unsanitized) AOT link+run : the exit code is unchanged by the fix
//     (a leak is not observable without a sanitizer), so LSan is the oracle
//     that moves; the plain assertions below pin that the fix did NOT change
//     any computed value — i.e. that nothing is freed early.
//
// ⚠ DELIBERATE EXCLUSIONS, each keeping the pre-existing leak (the safe
// direction), each confirmed to still leak 731 B / 40 allocations, and each
// filed as #2746:
//   * a RECORD receiver — §9/§9.7, a record's reference fields are borrows with
//     no independent +1, so a store would alias this reference;
//   * an INTERFACE-typed receiver — the runtime implementor may be
//     record-backed and, unlike #2540's *value* relaxation, there is no paired
//     call-site retain for the INDEX on the record-backed itable branch;
//   * an indexer ELEMENT that could ALIAS the index, **on the GETTER paths
//     only** — an inlined `get => k;` lowers to a bare load of the index, so
//     releasing would hand the caller a freed pointer. Confirmed load-bearing:
//     with the gate disabled the shape is an ASan `heap-use-after-free` in
//     `main`, not a leak. The SET / compound / `++` paths produce no escaping
//     accessor result and so are NOT excluded — case (Q) pins that a
//     `[k: string]: string` setter's index IS released.
//     ⚠ This gate is FAIL-CLOSED (round 3). It was once "is the element
//     `string`?", which was fail-OPEN: `Object` (a PrimitiveType that lowers to
//     the same ptrTy()), any `Kind::Named` such as `IEquatable<string>`, a bare
//     generic `T` reached through a `this` receiver (no substitution installed,
//     so it stays an unresolved `Kind::Named`), and a NULL `exprType` (an
//     imported indexer with no side-table entry) all fell THROUGH into the
//     release. Cases (R) and (S) pin two of those ((S) is a sema reject
//     since #4910, pending #4914); the `IEquatable<string>`
//     shape is a PRE-EXISTING heap-buffer-overflow independent of #2717 (it
//     reproduces identically on 2a990de) and is filed as #2752, so it cannot
//     serve as a regression test here; the null-`exprType` shape is covered by
//     construction (`!t` answers "may alias") rather than by a test;
//   * an index PINNED by a §22.3 accessor `ref`/`out` bracket — case (L).
//
// Cases (N)-(P) are CHARACTERIZATION guards for the first three. They cannot
// assert a clean LSan run (the shapes leak by design), so each asserts the
// PAIR: the checked oracle reports a LEAK and specifically NOT a
// use-after-free / double-free / SEGV, and the plain oracle returns the exact
// value. Delete a gate and the leak disappears -> red; turn one into a
// premature free and the report changes kind -> red. (A lit/FileCheck negative
// assertion on emitted IR would be the more direct instrument, but `bezelc` has
// no IR-emitting flag and the lit track is a diagnostics track; adding compiler
// CLI surface for a test is out of scope for a fix lane.)

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// #3733: the per-row `if (wasSkipped(r)) return;` guard that used to precede
// every `runAotChecked` row in this file is GONE, deliberately. A checked run is
// SKIPPED (not failed) when the ASan link toolchain is absent: `runAotChecked`
// returns ran=false with a "SKIP" diagnostic. Returning early on that literal
// reported the row as a PASS having run no sanitizer at all — a silent skip
// dressed as a clean result (issue #3733). Measured on this file with the
// harness's own knob (`BEZEL_AOT_FORCE_NO_TOOLCHAIN=1`): 22 rows "passed"
// with 22 skips and zero sanitizer runs.
//
// Every row below is an ACCEPT row: the program must COMPILE and RUN. So the
// runtime half already carries the whole verdict — `BEZEL_EXPECT_TRUE(r.ran)`
// (directly, or through `expectLeakButNoMemoryError`) sets `currentTestFailed()`
// when the run did not happen, and the row goes RED loudly instead of green.
// The guard was therefore dead weight that inverted the failure direction, and
// the right repair is to delete it, not to re-point it: `failedAtCompileTime-
// WithRealLocation(r)` asserts the opposite of what an accept row wants (that
// the driver REJECTED the program before running it).

// The concatenated diagnostic text of a checked run. runAotChecked funnels the
// program's whole sanitizer report into one Error diagnostic (aot_support.hpp
// ~1725, "#1822: surface any ASan report"), so a test can tell WHICH sanitizer
// fired — a leak is a very different verdict from a double free, and `r.ran` is
// false for both.
std::string sanitizerText(const JitResult& r) {
    std::string all;
    for (const auto& d : r.diagnostics.diagnostics()) all += d.message + "\n";
    return all;
}

// A checked run that is expected to LEAK (a documented #2746 residual) and, in
// particular, NOT to have freed anything early. Asserts the report is present
// and is a leak report, and that no memory-error report of any kind appears.
void expectLeakButNoMemoryError(const JitResult& r) {
    const std::string t = sanitizerText(r);
    BEZEL_EXPECT_TRUE(t.find("detected memory leaks") != std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("heap-use-after-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("double-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("SEGV") == std::string::npos);
}

// sum over i in 1..20 of len("<c>-{i}") = 9*3 + 11*4 = 71.
constexpr int kLenSum20 = 71;

// A read-write `[k: string]: int` indexer on a CLASS. The getter READS the
// index (`k.Length`) so the index cannot be optimized away; the setter assigns
// to NOTHING that involves `k`, so #2712's retain is not emitted and cannot be
// confused with this fix.
#define TABLE_CLASS                                                            \
    "module M; class Table "                                                   \
    "  public N: int; "                                                        \
    "  public property [k: string]: int "                                      \
    "    get => N + k.Length; "                                                \
    "    set => do "                                                           \
    "      N := value; "                                                       \
    "    end; "                                                                \
    "  end; "                                                                  \
    "end Table; "

// =====================================================================
// (A) THE ISSUE'S OWN REPRO — a NAMED LOCAL as the index of an inlined setter.
// The local is read AFTER the accessor call, so an over-release would be an
// observable use-after-free rather than a silent one. §23.4 / §28.9.
// =====================================================================
const char* kNamedLocalIndexInlinedSetter =
    TABLE_CLASS
    "class C "
    "  public static method Main(): int "
    "    using var t := new Table(); "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var s := $\"v-{i}\"; "
    "      t[s] := i; "
    "      total := total + s.Length; "     // reads s after the accessor
    "    end; "
    "    return total; "                    // 71
    "  end Main; "
    "end C;";

// =====================================================================
// (B) A `+1` PRODUCER TEMPORARY as the index of an inlined setter. Nothing owns
// it — no local, no callee. §23.5 / §28.1 / §28.9.
// =====================================================================
const char* kProducerTempIndexInlinedSetter =
    TABLE_CLASS
    "class C "
    "  public static method Main(): int "
    "    using var t := new Table(); "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      t[$\"w-{i}\"] := i; "
    "      total := total + i; "
    "    end; "
    "    return total; "                    // 210
    "  end Main; "
    "end C;";

// =====================================================================
// (C) The GETTER twin of (A) — the same escape-walk taint suppressed the
// local's release on a read, not only on a write. §23.4.
// =====================================================================
const char* kNamedLocalIndexInlinedGetter =
    TABLE_CLASS
    "class C "
    "  public static method Main(): int "
    "    using var t := new Table(); "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var s := $\"a-{i}\"; "
    "      total := total + t[s]; "
    "    end; "
    "    return total; "                    // 71 (N stays 0)
    "  end Main; "
    "end C;";

// =====================================================================
// (D) The GETTER twin of (B).
// =====================================================================
const char* kProducerTempIndexInlinedGetter =
    TABLE_CLASS
    "class C "
    "  public static method Main(): int "
    "    using var t := new Table(); "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + t[$\"b-{i}\"]; "
    "    end; "
    "    return total; "                    // 71
    "  end Main; "
    "end C;";

// =====================================================================
// (E) COMPOUND assignment `t[k] += 1` — lowerIndexerCompound evaluates the
// index ONCE and shares it between the getter and the setter (§23.4
// single-evaluation), so its +1 is discharged once, after the setter.
// Both index shapes. §6.6 / §23.4.
// =====================================================================
const char* kNamedLocalIndexCompound =
    TABLE_CLASS
    "class C "
    "  public static method Main(): int "
    "    using var t := new Table(); "
    "    for var i in 1..20 do "
    "      var s := $\"d-{i}\"; "
    "      t[s] += 1; "
    "    end; "
    "    return t.N; "                      // 71 + 20 = 91
    "  end Main; "
    "end C;";

const char* kProducerTempIndexCompound =
    TABLE_CLASS
    "class C "
    "  public static method Main(): int "
    "    using var t := new Table(); "
    "    for var i in 1..20 do "
    "      t[$\"c-{i}\"] += 1; "
    "    end; "
    "    return t.N; "                      // 91
    "  end Main; "
    "end C;";

// =====================================================================
// (F) The `++` indexer-slot path (§17.6's desugaring to the same get/set pair,
// LlvmBackend_Stmts.cpp) — the third site that lowers an index argument.
// =====================================================================
const char* kProducerTempIndexIncrement =
    TABLE_CLASS
    "class C "
    "  public static method Main(): int "
    "    using var t := new Table(); "
    "    for var i in 1..20 do "
    "      t[$\"e-{i}\"]++; "
    "    end; "
    "    return t.N; "                      // 91
    "  end Main; "
    "end C;";

// =====================================================================
// (G) The CLASS-VTABLE dispatch path — a `virtual`/`override` indexer reached
// through a base-typed receiver calls a COMPILED accessor rather than inlining
// one, so it is a distinct binding site. Both index shapes in one program.
// §23.4 ("Indexers … participate in `virtual`/`override`/`sealed`/`abstract`
// like methods").
// =====================================================================
const char* kVirtualIndexerVtableDispatch =
    "module M; class Base "
    "  public N: int; "
    "  public virtual property [k: string]: int "
    "    get => N + k.Length; "
    "    set => do "
    "      N := value; "
    "    end; "
    "  end; "
    "end Base; "
    "class Derived : Base "
    "  public override property [k: string]: int "
    "    get => N + k.Length + 0; "
    "    set => do "
    "      N := value; "
    "    end; "
    "  end; "
    "end Derived; "
    "class C "
    "  public static method Main(): int "
    "    using var d := new Derived(); "
    "    var b: Base := d; "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var s := $\"v-{i}\"; "
    "      b[s] := i; "                     // named local, vtable setter
    "      total := total + b[$\"w-{i}\"] - b.N; "  // temp, vtable getter
    "    end; "
    "    return total; "                    // 71
    "  end Main; "
    "end C;";

// =====================================================================
// (H) The BLOCK-FORM getter — lowered as a standalone function
// (ensureIndexerBlockGetter, §23.4 "Block-form indexers"), a fourth distinct
// binding site. Both index shapes.
// =====================================================================
const char* kBlockFormGetterIndex =
    "module M; class Table "
    "  public N: int; "
    "  public property [k: string]: int "
    "    get => do "
    "      return N + k.Length; "
    "    end; "
    "    set => do "
    "      N := value; "
    "    end; "
    "  end; "
    "end Table; "
    "class C "
    "  public static method Main(): int "
    "    using var t := new Table(); "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      total := total + t[$\"b-{i}\"]; "   // temp
    "      var s := $\"a-{i}\"; "
    "      total := total + t[s]; "           // named local
    "    end; "
    "    return total; "                    // 71 + 71 = 142
    "  end Main; "
    "end C;";

// =====================================================================
// (I) A MULTI-ARGUMENT indexer with a named local in one slot and a producer
// temporary in the other — the release must be per-index, not per-access.
// §23.4 (`property [row: int, col: int]`).
// =====================================================================
const char* kMultiArgIndexerMixedShapes =
    "module M; class Grid "
    "  public N: int; "
    "  public property [a: string, b: string]: int "
    "    get => N + a.Length + b.Length; "
    "    set => do "
    "      N := value; "
    "    end; "
    "  end; "
    "end Grid; "
    "class C "
    "  public static method Main(): int "
    "    using var g := new Grid(); "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var s := $\"a-{i}\"; "
    "      g[s, $\"b-{i}\"] := i; "
    "      total := total + g[s, $\"b-{i}\"] - g.N; "  // reads BOTH indices back
    "    end; "
    "    return total; "                    // 71 + 71 = 142
    "  end Main; "
    "end C;";

// =====================================================================
// (J) OVER-CORRECTION GUARD 1 — the caller-owned local must NOT be freed at the
// call site. AMPLIFIED to 64 iterations so at most the last could be spuriously
// register-reachable to LSan, and with an allocation CHURN between the accessor
// call and the read of `s`, so a premature free cannot read back an intact
// block. Were the producer-temporary release ever widened to identifier-shaped
// indices, this is an ASan `heap-use-after-free` on `s.Length`.
// len("v-1".."v-9") = 9*3, len("v-10".."v-64") = 55*4 -> 27 + 220 = 247.
// =====================================================================
const char* kCallerOwnedLocalNotFreedGuard =
    TABLE_CLASS
    "class C "
    "  public static method Main(): int "
    "    using var t := new Table(); "
    "    var total := 0; "
    "    for var i in 1..64 do "
    "      var s := $\"v-{i}\"; "
    "      t[s] := i; "
    "      var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "
    "      total := total + s.Length + churn.Length - churn.Length; "
    "    end; "
    "    return total; "                    // 247
    "  end Main; "
    "end C;";

// =====================================================================
// (K) OVER-CORRECTION GUARD 2 — the pairing that makes the release safe. The
// setter STORES the index into a class `string` field, which retains it
// (lowerStringFieldStore's `bz_string_retain` — the #429 argpass cardinal
// canary), so the field owns an independent +1. The caller then reads BOTH the
// local and the field. A missing retain would be a use-after-free; a doubled
// release would be a double-free; a spurious retain would be a leak — LSan and
// ASan are live oracles in all three directions. Both index shapes.
// The trailing `t.Last := "";` drops the field's last value onto an immortal
// sentinel so the program is fully balanced. §23.3 / §28.9.
// =====================================================================
#define STORING_TABLE_CLASS                                                    \
    "module M; class Table "                                                   \
    "  public Last: string := \"\"; "                                          \
    "  public property [k: string]: int "                                      \
    "    get => Last.Length; "                                                 \
    "    set => do "                                                           \
    "      Last := k; "                                                        \
    "    end; "                                                                \
    "  end; "                                                                  \
    "end Table; "

const char* kSetterStoresNamedLocalIndexIntoField =
    STORING_TABLE_CLASS
    "class C "
    "  public static method Main(): int "
    "    using var t := new Table(); "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var s := $\"v-{i}\"; "
    "      t[s] := 0; "
    "      var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "
    "      total := total + s.Length + t.Last.Length "
    "               + churn.Length - churn.Length; "
    "    end; "
    "    t.Last := \"\"; "
    "    return total; "                    // 71
    "  end Main; "
    "end C;";

const char* kSetterStoresProducerTempIndexIntoField =
    STORING_TABLE_CLASS
    "class C "
    "  public static method Main(): int "
    "    using var t := new Table(); "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      t[$\"v-{i}\"] := 0; "
    "      var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "
    "      total := total + t.Last.Length + churn.Length - churn.Length; "
    "    end; "
    "    t.Last := \"\"; "
    "    return total; "                    // 71
    "  end Main; "
    "end C;";

// =====================================================================
// (L) THE SEND-BACK CASE — a §22.3 accessor `ref` argument whose LOCATION is an
// indexer slot, with a `+1` producer index. `emitAccessorRefArgCopyIn` lowers
// each index ONCE and PINS it in `exprValueOverride_`, then drives BOTH
// lowerIndexerGet (the copy-in) and, after the call, lowerIndexerSet (the
// write-back) over that one pinned value — so an unsuppressed release fires
// TWICE (a double free; `emitStringRelease` is null/sentinel-safe but NOT
// idempotent) and the write-back setter reads the freed index (a UAF).
// Mechanically: 731 B / 40 allocations on 2a990de -> `SEGV` inside
// `__asan::Allocator::Deallocate` with the releases unsuppressed -> back to the
// documented leak with the suppression. Shaped after
// ref_out_accessor_writeback_1302_test.cpp's `ref_to_user_indexer_writes_back`
// with a `$"…"` index. N grows by len(k) + 5 each iteration: 71 + 100 = 171.
// =====================================================================
const char* kRefArgIntoIndexerSlotWithProducerIndex =
    TABLE_CLASS
    "class C "
    "  public static method Bump(ref a: int) a := a + 5; end Bump; "
    "  public static method Main(): int "
    "    using var t := new Table(); "
    "    for var i in 1..20 do "
    "      C.Bump(ref t[$\"k-{i}\"]); "
    "    end; "
    "    return t.N; "                      // 171
    "  end Main; "
    "end C;";

// (M) the `out` sibling — no copy-in getter runs, so pre-suppression it fired
// exactly once and was ACCIDENTALLY correct. The suppression is uniform rather
// than ref-only (the helper cannot see the modifier, and relying on an
// accidental single-fire is fragile), so `out` now takes the same documented
// leak. Recorded in #2746.
const char* kOutArgIntoIndexerSlotWithProducerIndex =
    TABLE_CLASS
    "class C "
    "  public static method Fill(out a: int) a := 7; end Fill; "
    "  public static method Main(): int "
    "    using var t := new Table(); "
    "    for var i in 1..20 do "
    "      C.Fill(out t[$\"m-{i}\"]); "
    "    end; "
    "    return t.N; "                      // 7
    "  end Main; "
    "end C;";

// =====================================================================
// (N)-(P) CHARACTERIZATION guards for the three deliberate exclusions.
// =====================================================================
const char* kRecordReceiverResidual =
    "module M; record Row "
    "  public N: int; "
    "  public property [k: string]: int "
    "    get => N + k.Length; "
    "    set => do "
    "      N := value; "
    "    end; "
    "  end; "
    "end Row; "
    "class C "
    "  public static method Main(): int "
    "    var r := new Row(0); "
    "    var n := 0; "
    "    for var i in 1..20 do "
    "      r[$\"z-{i}\"] := i; "
    "      n := n + 1; "
    "    end; "
    "    return n; "                        // 20
    "  end Main; "
    "end C;";

const char* kInterfaceReceiverResidual =
    "module M; interface IBag "
    "  property [k: string]: int; "
    "end IBag; "
    "class Bag : IBag "
    "  public N: int; "
    "  public property [k: string]: int "
    "    get => N + k.Length; "
    "    set => do "
    "      N := value; "
    "    end; "
    "  end; "
    "end Bag; "
    "class C "
    "  public static method Main(): int "
    "    using var b := new Bag(); "
    "    var f: IBag := b; "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var s := $\"v-{i}\"; "
    "      f[s] := i; "
    "      var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "
    "      total := total + s.Length + churn.Length - churn.Length; "
    "    end; "
    "    return total; "                    // 71
    "  end Main; "
    "end C;";

// The GETTER of a `string`-valued indexer that yields the index itself. This is
// the shape whose gate is load-bearing: with the element-type exclusion removed
// the `got.Length` read below is an ASan `heap-use-after-free` in `main`.
const char* kStringElementGetterResidual =
    "module M; class Echo "
    "  public property [k: string]: string "
    "    get => k; "
    "  end; "
    "end Echo; "
    "class C "
    "  public static method Main(): int "
    "    using var e := new Echo(); "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var got := e[$\"y-{i}\"]; "
    "      var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "
    "      total := total + got.Length + churn.Length - churn.Length; "
    "    end; "
    "    return total; "                    // 71
    "  end Main; "
    "end C;";

// =====================================================================
// (Q) The SET path of a `string`-valued indexer IS released — no accessor
// result reaches the caller there, so the element-type exclusion does not
// apply. This shape leaked under a blanket element-type gate; it is clean now.
// §23.4 / §28.9.
// =====================================================================
const char* kStringElementSetterIndexIsReleased =
    "module M; class Store "
    "  public Last: string := \"\"; "
    "  public property [k: string]: string "
    "    get => Last; "
    "    set => do "
    "      Last := k; "                    // stores the INDEX; class field
    "    end; "                            // store RETAINS (§23.3)
    "  end; "
    "end Store; "
    "class C "
    "  public static method Main(): int "
    "    using var t := new Store(); "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      t[$\"s-{i}\"] := \"x\"; "
    "      var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "
    "      total := total + t.Last.Length + churn.Length - churn.Length; "
    "    end; "
    "    t.Last := \"\"; "
    "    return total; "                    // 71
    "  end Main; "
    "end C;";

// =====================================================================
// (R) An `Object`-typed element. `Object` accepts any source
// (ExprTypecheck.cpp ~2964, "`Object` (the root) accepts anything — never
// flag") and NO conversion is emitted between the inlined getter body and the
// release, so `get => k;` yields the index pointer itself.
// =====================================================================
const char* kObjectElementResidual =
    "module M; class Echo "
    "  public property [k: string]: Object "
    "    get => k; "
    "  end; "
    "end Echo; "
    "class C "
    "  public static method Main(): int "
    "    using var e := new Echo(); "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var o: Object := e[$\"y-{i}\"]; "
    "      var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "
    "      total := total + o.ToString().Length + churn.Length - churn.Length; "
    "    end; "
    "    return total; "                    // 71
    "  end Main; "
    "end C;";

// (S) A bare generic `T` element reached through a `this` receiver. The value
// crosses back to a caller that READS it as a `string`, which is what makes a
// premature free observable rather than latent. Since #4910 this program is a
// REJECT pin: `get => k;` returns a `string` as an unconstrained `T`, which
// has no §5.1 conversion (v0.307). The codegen half waits on #4914.
const char* kBareGenericElementViaThisResidual =
    "module M; class Box<T> "
    "  public property [k: string]: T "
    "    get => k; "
    "  end; "
    "  public method Get(i: int): T "
    "    return this[$\"x-{i}\"]; "
    "  end Get; "
    "end Box; "
    "class C "
    "  public static method Main(): int "
    "    using var b := new Box<string>(); "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var s: string := b.Get(i); "
    "      var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "
    "      total := total + s.Length + churn.Length - churn.Length; "
    "    end; "
    "    return total; "                    // 71
    "  end Main; "
    "end C;";

// =====================================================================
// (T) / (U) THE CROSSED AXIS — a NAMED LOCAL index into an indexer whose
// ELEMENT is pointer-shaped, with the result reaching a sink that outlives the
// local WITHOUT retaining. Every earlier pointer-element case used a TEMPORARY
// index and every earlier named-local case used an `int` element, so this cell
// — the only one where the escape-walk half can free a value the caller still
// holds — was never covered.
//
// The sink is the load-bearing part. `lowerStringLocalReassign` declines a
// `string?` slot and any non-string target, leaving a plain `CreateStore` that
// takes no reference — so `last` aliases the index that the walker relaxation
// then releases at the local's scope end. Pre-fix: ASan `heap-use-after-free`;
// 2a990de merely leaked 694 B / 38 allocations. Returns len("y-20") = 4.
// §4.6 / §28.9 / §7 (nullable).
// =====================================================================
const char* kNamedLocalIndexPointerElementNullableSink =
    "module M; class Echo "
    "  public property [k: string]: string "
    "    get => k; "
    "  end; "
    "end Echo; "
    "class C "
    "  public static method Main(): int "
    "    using var e := new Echo(); "
    "    var last: string? := null; "
    "    for var i in 1..20 do "
    "      var s := $\"y-{i}\"; "
    "      last := e[s]; "                  // plain store: raw alias, no retain
    "      var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "
    "      if churn.Length < 0 then return 99; end; "
    "    end; "
    "    return last!.Length; "             // 4
    "  end Main; "
    "end C;";

// (U) the same defect without needing nullability — an `Object`-typed element
// and an `Object` sink, which is likewise a non-string target for
// lowerStringLocalReassign.
const char* kNamedLocalIndexObjectElementSink =
    "module M; class Echo "
    "  public property [k: string]: Object "
    "    get => k; "
    "  end; "
    "end Echo; "
    "class C "
    "  public static method Main(): int "
    "    using var e := new Echo(); "
    "    var last: Object := 0; "
    "    for var i in 1..20 do "
    "      var s := $\"y-{i}\"; "
    "      last := e[s]; "
    "      var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "
    "      if churn.Length < 0 then return 99; end; "
    "    end; "
    "    return last.ToString().Length; "   // 4
    "  end Main; "
    "end C;";

}  // namespace

// ---------------------------------------------------------------------
// (A) and its PLAIN-path twin.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_index_arg_release_2717,
           named_local_index_arg_to_inlined_setter_spec_23_4) {
    JitResult r = runAotChecked(kNamedLocalIndexInlinedSetter);
    BEZEL_EXPECT_TRUE(r.ran);              // 731 B / 40 allocations pre-fix
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_string_index_arg_release_2717,
           named_local_index_arg_plain_path_value_invariant_spec_23_4) {
    // NOT a #2717 oracle — a leak is invisible without a sanitizer, so this
    // case passes with and without the fix. It is a VALUE-INVARIANCE check
    // only: it has no allocation churn between the accessor and the read, so a
    // freed chunk can read back intact. The strong over-release oracle for this
    // shape is the 64-iteration + churn case (J) below; this one exists to pin
    // that the issue's own repro still computes 71 verbatim.
    JitResult r = runAot(kNamedLocalIndexInlinedSetter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

// ---------------------------------------------------------------------
// (B) the producer temporary.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_index_arg_release_2717,
           producer_temp_index_arg_to_inlined_setter_spec_28_9) {
    JitResult r = runAotChecked(kProducerTempIndexInlinedSetter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 210);
}

BEZEL_TEST(codegen_string_index_arg_release_2717,
           producer_temp_index_arg_plain_path_value_invariant_spec_28_9) {
    JitResult r = runAot(kProducerTempIndexInlinedSetter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 210);
}

// ---------------------------------------------------------------------
// (C) / (D) the getter side.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_index_arg_release_2717,
           named_local_index_arg_to_inlined_getter_spec_23_4) {
    JitResult r = runAotChecked(kNamedLocalIndexInlinedGetter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_string_index_arg_release_2717,
           producer_temp_index_arg_to_inlined_getter_spec_23_4) {
    JitResult r = runAotChecked(kProducerTempIndexInlinedGetter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

// ---------------------------------------------------------------------
// (E) compound assignment, both index shapes.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_index_arg_release_2717,
           named_local_index_arg_to_compound_assign_spec_6_6) {
    JitResult r = runAotChecked(kNamedLocalIndexCompound);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 91);
}

BEZEL_TEST(codegen_string_index_arg_release_2717,
           producer_temp_index_arg_to_compound_assign_spec_6_6) {
    JitResult r = runAotChecked(kProducerTempIndexCompound);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 91);
}

// ---------------------------------------------------------------------
// (F) the `++` indexer-slot path.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_index_arg_release_2717,
           producer_temp_index_arg_to_increment_spec_23_4) {
    JitResult r = runAotChecked(kProducerTempIndexIncrement);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 91);
}

// ---------------------------------------------------------------------
// (G) the class-vtable dispatch path.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_index_arg_release_2717,
           index_args_through_virtual_indexer_vtable_spec_23_4) {
    JitResult r = runAotChecked(kVirtualIndexerVtableDispatch);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

// ---------------------------------------------------------------------
// (H) the block-form getter (standalone compiled accessor).
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_index_arg_release_2717,
           index_args_to_block_form_getter_spec_23_4) {
    JitResult r = runAotChecked(kBlockFormGetterIndex);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2 * kLenSum20);   // both index shapes read back
}

// ---------------------------------------------------------------------
// (I) the multi-argument indexer.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_index_arg_release_2717,
           multi_arg_indexer_mixed_index_shapes_spec_23_4) {
    JitResult r = runAotChecked(kMultiArgIndexerMixedShapes);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2 * kLenSum20);   // BOTH indices read back
}

// ---------------------------------------------------------------------
// (J) OVER-CORRECTION GUARD 1 — a caller-owned local is never freed here.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_index_arg_release_2717,
           caller_owned_local_index_is_not_freed_at_the_call_site_spec_28_9) {
    JitResult r = runAotChecked(kCallerOwnedLocalNotFreedGuard);
    BEZEL_EXPECT_TRUE(r.ran);   // over-release ⇒ heap-use-after-free
    BEZEL_EXPECT_EQ(r.exitCode, 247);
}

BEZEL_TEST(codegen_string_index_arg_release_2717,
           caller_owned_local_index_guard_plain_path_spec_28_9) {
    JitResult r = runAot(kCallerOwnedLocalNotFreedGuard);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 247);
}

// ---------------------------------------------------------------------
// (K) OVER-CORRECTION GUARD 2 — the class-field retain that pairs the release.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_index_arg_release_2717,
           setter_storing_named_local_index_into_field_stays_balanced_spec_23_3) {
    JitResult r = runAotChecked(kSetterStoresNamedLocalIndexIntoField);
    BEZEL_EXPECT_TRUE(r.ran);   // double-free / UAF / leak all fail here
    // s.Length + t.Last.Length, both non-cancelling: the field read is a live
    // oracle for the value the setter stored.
    BEZEL_EXPECT_EQ(r.exitCode, 2 * kLenSum20);
}

BEZEL_TEST(codegen_string_index_arg_release_2717,
           setter_storing_temp_index_into_field_stays_balanced_spec_23_3) {
    JitResult r = runAotChecked(kSetterStoresProducerTempIndexIntoField);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

// ---------------------------------------------------------------------
// (L) / (M) §22.3 accessor `ref`/`out` argument whose LOCATION is an indexer
// slot — the pinned index must be released EXACTLY ONCE, by the §22.3
// bracket itself (NOT by either accessor). #2717's first cut released at both
// ends of the copy-in/copy-out bracket (a double free + use-after-free
// observed as `SEGV` inside `__asan::Allocator::Deallocate`); suppressing at
// both ends restored the leak. #2746 closes the leak by relocating the
// release into the bracket so it fires once, after the call, on both the
// normal-return and exception edges.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_index_arg_release_2717,
           ref_arg_into_indexer_slot_index_is_released_exactly_once_spec_22_3) {
    JitResult r = runAotChecked(kRefArgIntoIndexerSlotWithProducerIndex);
    // #2746 fix: the bracket owns the lifetime across copy-in + write-back,
    // so the release fires once, here, instead of at EITHER accessor (which
    // would double-free). Pre-fix this asserted LEAK; the assertion inverts
    // here because the leak has been closed. Pre-fix: r.ran=false (LSan
    // reports the leak), post-fix: r.ran=true and exitCode=171.
    BEZEL_EXPECT_TRUE(r.ran);                  // #2746 closes the leak
    BEZEL_EXPECT_EQ(r.exitCode, 171);         // §22.3 write-back value preserved
    // Belt-and-suspenders: even after r.ran=true, re-grep the report for the
    // specific sanitizer-failure signatures — a double-free / UAF would still
    // set `r.ran=false`, but the gate fires FIRST so this confirms the FAIL
    // direction (the leak that was the pre-fix contract has been removed,
    // NOT replaced by a memory error).
    const std::string t = sanitizerText(r);
    BEZEL_EXPECT_TRUE(t.find("detected memory leaks") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("heap-use-after-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("double-free") == std::string::npos);
}

BEZEL_TEST(codegen_string_index_arg_release_2717,
           ref_arg_into_indexer_slot_writes_back_correctly_spec_22_3) {
    // The plain oracle: §22.3's copy-in/copy-out must still compute 171. The
    // double free did NOT change this value, which is why the checked case
    // above — not this one — is the regression oracle.
    JitResult r = runAot(kRefArgIntoIndexerSlotWithProducerIndex);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 171);
}

BEZEL_TEST(codegen_string_index_arg_release_2717,
           out_arg_into_indexer_slot_index_is_released_exactly_once_spec_22_3) {
    JitResult r = runAotChecked(kOutArgIntoIndexerSlotWithProducerIndex);
    // Same as the `ref` case above. Pre-fix this asserted LEAK; #2746 closes
    // it. `out` was ACCIDENTALLY clean before #2717 (no copy-in getter runs,
    // so exactly one release fired and the helper happened to be correct);
    // the uniform suppression at both ends made it leak in lockstep with
    // `ref`. #2746's bracket release restores `out` to its pre-#2717 leak-
    // free behavior, but for the right reason now: the §22.3 bracket itself
    // owns the lifetime.
    BEZEL_EXPECT_TRUE(r.ran);                  // #2746 closes the leak
    BEZEL_EXPECT_EQ(r.exitCode, 7);
    const std::string t = sanitizerText(r);
    BEZEL_EXPECT_TRUE(t.find("detected memory leaks") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("heap-use-after-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("double-free") == std::string::npos);
}

BEZEL_TEST(codegen_string_index_arg_release_2717,
           out_arg_into_indexer_slot_writes_back_correctly_spec_22_3) {
    // §22.3 write-back oracle only: `Fill` stores a constant, so this value is
    // 7 whatever happens to the index string. The memory oracle for this shape
    // is the checked case above.
    JitResult r = runAot(kOutArgIntoIndexerSlotWithProducerIndex);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ---------------------------------------------------------------------
// (N)-(P) the three deliberate exclusions, pinned as characterization guards.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_index_arg_release_2717,
           record_receiver_index_is_deliberately_not_released_spec_9_7) {
    JitResult r = runAotChecked(kRecordReceiverResidual);
    expectLeakButNoMemoryError(r);
}

BEZEL_TEST(codegen_string_index_arg_release_2717,
           record_receiver_residual_value_is_correct_spec_9_7) {
    // A bare counter: it pins that the program runs to completion, not that the
    // index survived. The memory oracle is the checked case above.
    JitResult r = runAot(kRecordReceiverResidual);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 20);
}

BEZEL_TEST(codegen_string_index_arg_release_2717,
           interface_receiver_index_is_deliberately_not_released_spec_11) {
    JitResult r = runAotChecked(kInterfaceReceiverResidual);
    expectLeakButNoMemoryError(r);
}

BEZEL_TEST(codegen_string_index_arg_release_2717,
           interface_receiver_residual_value_is_correct_spec_11) {
    JitResult r = runAot(kInterfaceReceiverResidual);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_string_index_arg_release_2717,
           string_element_getter_index_is_deliberately_not_released_spec_23_4) {
    // The load-bearing gate: without it this is `heap-use-after-free` in main,
    // which expectLeakButNoMemoryError rejects explicitly.
    JitResult r = runAotChecked(kStringElementGetterResidual);
    expectLeakButNoMemoryError(r);
}

BEZEL_TEST(codegen_string_index_arg_release_2717,
           string_element_getter_residual_value_is_correct_spec_23_4) {
    JitResult r = runAot(kStringElementGetterResidual);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

// ---------------------------------------------------------------------
// (Q) the SET path of a string-valued indexer IS released — no accessor result
// reaches the caller there, so the element-type exclusion does not apply.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_index_arg_release_2717,
           string_element_setter_index_is_released_spec_23_4) {
    // A live oracle in three directions, not a counter: the setter stores the
    // INDEX into a class `string` field (which retains, §23.3) and the caller
    // reads that field back with allocation churn in between. A missing release
    // leaks; a doubled one double-frees; releasing before the field's retain is
    // a UAF on `t.Last.Length`.
    JitResult r = runAotChecked(kStringElementSetterIndexIsReleased);
    BEZEL_EXPECT_TRUE(r.ran);   // leaked under a blanket element-type gate
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

// ---------------------------------------------------------------------
// (R) / (S) FAIL-CLOSED element gate (round 3). Two element types that are
// pointer-shaped but not spelled `string`, each of which an inlined `get => k;`
// can hand back as the index itself. Under the earlier `primitive == String`
// test both fell THROUGH into the release; both are now declined.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_index_arg_release_2717,
           object_element_index_is_not_released_spec_23_4) {
    // `Object` is a PrimitiveType here (§4: "treated as a primitive lookup
    // key") and lowers to the same ptrTy() as `string`, so the alias is silent
    // and verifyModule stays clean. Under the fail-open gate this was an ASan
    // `heap-use-after-free` in main; 2a990de merely leaked.
    //
    // ⚠ WHAT MAKES THIS CASE NON-VACUOUS, and how it could stop being so:
    // `o.ToString()` on an `Object` receiver routes to lowerObjectToString,
    // which dereferences word 0 and returns the receiver BY IDENTITY
    // (LlvmBackend_String.cpp ~2852) — two reads of the freed block. If
    // `Object.ToString()` ever grows a copy step, this stops observing the free
    // and needs a different sink.
    JitResult r = runAotChecked(kObjectElementResidual);
    expectLeakButNoMemoryError(r);
}

BEZEL_TEST(codegen_string_index_arg_release_2717,
           object_element_residual_value_is_correct_spec_23_4) {
    JitResult r = runAot(kObjectElementResidual);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}

BEZEL_TEST(codegen_string_index_arg_release_2717,
           bare_generic_element_via_this_receiver_is_rejected_spec_5_1) {
    // fix #4910: this program is ILL-TYPED, so it is pinned as a REJECT.
    // The getter `get => k;` returns `k: string` as the unconstrained element
    // type `T`. §5.1 (v0.307) has no implicit conversion from `string` (or any
    // primitive) to a type parameter; the only conversions out of a `T` go to
    // `Object` and to its constraint bounds. §3.6 rule 2 says a generic body
    // type-checks under the weakest assumption its constraints establish. So
    // 64c0d712 (#4810) rejects the getter's return, correctly. Before that
    // commit the program compiled, and these two rows asserted
    // `expectLeakButNoMemoryError` + exit 71 on it.
    //
    // The program text is unchanged, so this row pins the original repro.
    // The codegen pin it used to carry (a bare-`T` element via a `this`
    // receiver must not be released, the #2717 fail-closed element gate) has
    // no well-typed program yet: the legal spelling `get => (k as T)!;`
    // degrades to a null return (#2491 residual). Restore the leak-but-no-
    // memory-error and value rows on that spelling once #4914 lands.
    //
    // fix #4916: the #4810 `checkAssignable` arm's diagnostic was rewritten
    // for the GENERAL arm (return / argument / assignment) so it does not
    // falsely claim "a primitive literal cannot initialize a generic field"
    // (no literal here, and this site is not a field). The new wording
    // names the source primitive type and cites §3.6 rule 2 (§5.1, §14.2
    // remain). The visitMember FieldDecl / PropertyDecl arms keep the OLD
    // wording — those sites are the field case the OLD wording describes.
    JitResult r = runAot(kBareGenericElementViaThisResidual);
    BEZEL_EXPECT_TRUE(!r.ran);
    // The #4810 / #4916 general-arm wording — `string` is the source
    // primitive (`k: string`) and `T` is the type parameter the getter
    // declares. Unique to the GENERAL arm: the visitMember arms prepend
    // "initializer for …" and don't say "value of primitive type".
    const std::string needle =
        "a value of primitive type `string` does not implicitly convert to "
        "the type parameter `T`";
    // The program is one source line; the diagnostic must sit on the `k` of
    // `get => k;` (1-based column), not merely somewhere in the file. runAot
    // surfaces the compiler's "<path>:<line>:<col>: " prefix in the message
    // text (the structured location is 0:0), so the position is read there.
    const std::string prog = kBareGenericElementViaThisResidual;
    const std::size_t getAt = prog.find("get => k;");
    BEZEL_EXPECT_TRUE(getAt != std::string::npos);
    const std::string wantPos =
        ":1:" + std::to_string(getAt + std::string("get => ").size() + 1) +
        ": ";
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) == std::string::npos) continue;
        if (d.message.find(wantPos) != std::string::npos) found = true;
    }
    if (!found)
        for (const auto& d : r.diagnostics.diagnostics())
            std::fprintf(stderr, "    [%u:%u] %s\n", d.location.line,
                         d.location.column, d.message.c_str());
    BEZEL_EXPECT_TRUE(found);
}

// ---------------------------------------------------------------------
// (T) / (U) the crossed axis: named-local index x pointer-shaped element.
// The escape-walk half must NOT arm the local's scope-end release when the
// accessor can hand that local's string back to a non-retaining sink.
// ---------------------------------------------------------------------
BEZEL_TEST(codegen_string_index_arg_release_2717,
           named_local_index_with_string_element_is_not_armed_spec_4_6) {
    JitResult r = runAotChecked(kNamedLocalIndexPointerElementNullableSink);
    expectLeakButNoMemoryError(r);
}

BEZEL_TEST(codegen_string_index_arg_release_2717,
           named_local_index_with_string_element_value_survives_spec_4_6) {
    JitResult r = runAot(kNamedLocalIndexPointerElementNullableSink);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);
}

BEZEL_TEST(codegen_string_index_arg_release_2717,
           named_local_index_with_object_element_is_not_armed_spec_4_6) {
    JitResult r = runAotChecked(kNamedLocalIndexObjectElementSink);
    expectLeakButNoMemoryError(r);
}

BEZEL_TEST(codegen_string_index_arg_release_2717,
           named_local_index_with_object_element_value_survives_spec_4_6) {
    JitResult r = runAot(kNamedLocalIndexObjectElementSink);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 4);
}
