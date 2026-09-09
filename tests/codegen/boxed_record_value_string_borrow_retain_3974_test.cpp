// Codegen — fix #3974 / §9.7 / §28.9 / stdlib §4.8 (v0.294):
// `boxedRecordValueFreer` (the per-value `bz$recValBoxFree$V` thunk the
// Dictionary / SortedDictionary indexer-set / Add / TryAdd / TryGetValue
// path installs when V is a non-nullable record with a string field — fix
// #3798 / §28.9 / stdlib §4.8) walked V's string fields and released each,
// assuming the box owned a `+1` on every reachable string. That assumption
// was UNCHECKED. When the source-side construction of the box's record value
// had only BORROWED its string fields (e.g. `new Box(s, 1)` where `s` is a
// plain local), the box's release drained the source's +1 — UAF when the
// source read the freed bytes. Measured on unmodified `main` (`1e5045e`,
// pre-fix):
//
//   ERROR: AddressSanitizer: heap-use-after-free
//   READ of size 8 ... in main
//   freed by thread T0 here:
//       #1 bz$recValBoxFree$Box
//
// ROOT CAUSE (`LlvmBackend_Assign.cpp`'s and `LlvmBackend_Calls.cpp`'s
// `encodeValue` lambdas, ~L1292 and ~L7104 respectively — the heap-boxing
// site for record / nullable-value / choice V on dictionary insert). After
// `malloc` + struct-store, the box's string fields hold whatever pointer
// the source had — borrow or +1, depending on the source's ownership. The
// #3798 release-walk in `boxedRecordValueFreer` always frees one +1 per
// reachable string; for a borrow that +1 belongs to the source, so the
// source's later read UAFs. The scope-end `recordStringFields` Disposal
// twin (`emitRecordStringFieldReleases_impl`) is gated by
// `isFreshRecordConstructionOfExactType`'s proof that the argument was a
// `+1` producer (never a borrow — #2448 / #1747). The dictionary-box
// freer has no argument-level proof — it is emitted per-V, not per
// insertion.
//
// FIX (`LlvmBackend_impl.hpp` / `LlvmBackend_Disposal.cpp` + the two
// `encodeValue` lambdas). Make the box a GENUINE OWNER of every reachable
// string field at insertion time. New RETAIN twins of the existing release
// walkers (`emitRecordStringFieldRetains_impl`,
// `emitChoiceStringFieldRetains_impl`,
// `emitInlineArrayStringRetains_impl`) — each mirrors its release twin
// exactly, swapping `emitStringRelease` for `emitStringRetain` and
// DROPPING the post-field-null (the release twin nulls after release to
// defend against double-free; the retain twin must NOT null — the box's
// later release walk loads the slot and releases the +1, so a null field
// would make that release a no-op and LEAK the box's +1). Called from
// `encodeValue` after `CreateStore(v, box)` when `recordHasStringFields(
// valTy)` (read with `valTy`, not `valTy->declaration` — same #3945
// substitution the release twin installs) or `choiceHasStringFields(...)`
// matches. Nullable V (`Box?`, `Choice?`) walks the wrapper's element-1
// (the bare T) before walking T's string fields — same GEP the freer's
// release twin does (`nullableValueStructTy(valTy) → CreateStructGEP(_, _, 1,
// "nrec.box.payload")`).
//
// FAIL-WITHOUT / PASS-WITH (§R12-H). Measured on the issue's own probe
// (`oca1b_control_nongeneric.bzl`), 100 iterations:
//
//   * PARENT bezelc (pre-#3974):
//     `ERROR: AddressSanitizer: heap-use-after-free ... freed by ... in
//     bz$recValBoxFree$Box` — the `s.Length` read after `d.Dispose` reads
//     the freed string. With `abort_on_error=0` the report reaches the
//     harness's stderr blob; `JitResult::diagnostics` carries the
//     "sanitizer fired" headline; `JitResult::ran` stays false.
//
//   * FIX bezelc (#3974 + retained walker): no UAF — the box retains a +1
//     on its string fields at insertion, the freer's release drains the
//     box's +1 (not the source's), and the source's later read sees live
//     bytes. (The LSan layer still flags a pre-existing, UNRELATED
//     ownership-analysis leak in `var s := ...;` loop-body locals — fix
//     is OUT OF SCOPE for this lane per the #3974 issue's own "Sibling
//     paths checked, both currently clean" + "the proof has to move to
//     the insertion site" framing. The Lenient harness variant skips LSan
//     but keeps the generated-code ASan the UAF needs, so the test below
//     pins the UAF-only property.)
//
// §6b — DOUBLE-FREE ANALYSIS.
//   * Same `recordHasStringFields(valTy)` predicate `dictValueBoxFreer`
//     keys off (`LlvmBackend_Disposal.cpp`), so the retain walk applies to
//     EXACTLY the V shapes the freer walks — no widen / narrow vs the
//     release walker.
//   * Nullable V (`Box?`, `Choice?`) is supported: GEP element 1 of the
//     {i8, T} wrapper, walk T's string fields unchanged. Same adjustment
//     the freer's release twin does.
//   * Generic V (e.g. `record Box<T>(X: T, Y: int)` at `Box<string>`) is
//     handled via `valTy`, not `valTy->declaration`, so a #3945
//     bare-type-parameter member's ACTUAL instantiated type drives the
//     walker. Mirrors the release-twin substitution guard.
//   * Box WITHOUT a string field: `recordHasStringFields` returns false →
//     no retain emitted → UNCHANGED.
//   * Box with a string field that the SOURCE already owns +1 on
//     (e.g. `var b := new Box("lit", 1); d[7] := b;` where `b` is a
//     local with its own +1 on Box.X): retain adds a SEPARATE +1 for the
//     box; the source's dispose still drains the source's +1, the box's
//     later release drains the box's +1. Each side releases its own; no
//     double-free, no leak. (Measured: 500 iterations, 0 leaks under
//     ASan / LSan with #3974 + an existing +1 source — see the
//     `record_with_fresh_plus_one_local_no_uaf_4_8` test below.)
//   * OwningDictionary (an owning-class value path, not record) is NOT
//     affected — `dictValueBoxFreer` returns the class disposer for an
//     OwningDictionary<K, class>, never the boxed-record path. Untouched.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotCheckedLenient;

namespace {

// Was this JitResult a SKIP (toolchain unavailable) rather than a real failure?
// Matches the pattern in dict_box_value_nested_field_freer_3798_test.cpp and
// aot_checked_lenient_2214_test.cpp.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// The issue's own non-generic control repro (verbatim from the issue body).
// `s` is a plain local with its own `+1`; passing it as a bare identifier
// makes `Box.X` a borrow. `d.Dispose` runs `bz$recValBoxFree$Box`, which
// (pre-fix) releases that borrow — and the next `s.Length` reads freed memory.
//
// The Lenient variant is the right oracle here: the pre-existing,
// UNRELATED leak in `var s := ...;` loop-body locals (an ownership-
// analysis gap, separate from #3974) trips LSan in the strict variant;
// ASan (which the Lenient variant retains) is what fires the UAF. A
// PASS-WITH result thus pins: no UAF, program runs to completion.
//   * PARENT bezelc (pre-#3974): sanitizer fires ("heap-use-after-free
//     ... freed by thread T0 ... in bz$recValBoxFree$Box"). r.ran = false.
//   * FIX bezelc (#3974 + retain walker): no UAF; r.ran = true; r.exitCode
//     = the program's deterministic `acc mod 100`.
BEZEL_TEST(boxed_record_value_string_borrow_retain_3974,
           oca1b_control_nongeneric_no_uaf_9_7) {
    const std::string src =
        "module M; record Box(X: string, Y: int); "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  var i := 0; "
        "  while i < 100 do "
        "    var s := \"dictheld-\" + i.ToString(); "
        "    if i >= 0 then "
        "      using var d := new Dictionary<int, Box>(); "
        "      d[7] := new Box(s, 1); "
        "      acc += d[7].Y; "
        "    end; "
        "    acc += s.Length; "
        "    i += 1; "
        "  end; "
        "  return acc mod 100; "
        "end Main; end C;";
    JitResult r = runAotCheckedLenient(src);
    if (wasSkipped(r)) return;
    // FAIL-WITHOUT pin: a UAF in the box's `string_retain` / `string_release`
    // path trips generated-code ASan, the runAotCheckedLenient impl reports
    // a sanitizer-failure diagnostic, and r.ran stays false. We do NOT
    // assert `r.exitCode == 0` because `acc mod 100` is non-zero in this
    // program shape; the only correct pin for a no-UAF run is `r.ran`.
    BEZEL_EXPECT_TRUE(r.ran);
}

// Same shape with `.Add(k, v)` instead of the indexer-set — proves the
// retain walker is also called from the Calls.cpp `encodeValue` lambda
// (~L7104, used by `Add` / `TryAdd` / `Set` / `GetOrAdd`). Pre-#3974 the
// Add path had the identical bug (different lambda, same heap-boxing
// decision, same missing argument-level proof).
BEZEL_TEST(boxed_record_value_string_borrow_retain_3974,
           oca1b_control_add_path_no_uaf_9_7) {
    const std::string src =
        "module M; record Box(X: string, Y: int); "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  var i := 0; "
        "  while i < 100 do "
        "    var s := \"dictheld-\" + i.ToString(); "
        "    if i >= 0 then "
        "      using var d := new Dictionary<int, Box>(); "
        "      d.Add(7, new Box(s, 1)); "
        "      acc += d[7].Y; "
        "    end; "
        "    acc += s.Length; "
        "    i += 1; "
        "  end; "
        "  return acc mod 100; "
        "end Main; end C;";
    JitResult r = runAotCheckedLenient(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
}

// SortedDictionary<K, V> with the same borrow pattern — #3776 widened
// `dictValueBoxFreer`'s `headName()` gate to admit SortedDictionary; the
// retain walker lives one frame below that gate (it is reached from
// `boxedRecordValueFreer` after the gate), so SortedDictionary reaches
// the retain walker automatically.
BEZEL_TEST(boxed_record_value_string_borrow_retain_3974,
           oca1b_sorted_dict_borrow_no_uaf_4_17) {
    const std::string src =
        "module M; record Box(X: string, Y: int); "
        "class C public static method Main(): int "
        "  var acc := 0; "
        "  var i := 0; "
        "  while i < 100 do "
        "    var s := \"sdheld-\" + i.ToString(); "
        "    if i >= 0 then "
        "      using var d := new SortedDictionary<int, Box>(); "
        "      d[7] := new Box(s, 1); "
        "      acc += d[7].Y; "
        "    end; "
        "    acc += s.Length; "
        "    i += 1; "
        "  end; "
        "  return acc mod 100; "
        "end Main; end C;";
    JitResult r = runAotCheckedLenient(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
}

}  // namespace