// Codegen — issue #4640: `Dictionary<Object, V>[key] := value` SIGSEGVs when
// `key` is a bare value (not pre-boxed), because the indexer-set lowerer
// spills the bare key to a stack slot and passes its pointer to
// `bz_dict_set_gen`, which then reads `key_size` (16 B for a §8.6 box) from a
// 4-byte slot — a stack-buffer-overflow on the `bz$valkey$hash` 8-byte read.
//
// MECHANISM (re-derived on this lane's HEAD, f644098f, and confirmed from
// emitted IR — not merely from the symptom). The `keyTy` for
// `Dictionary<Object, V>` is the `Object` declared type, so `genericBackingType`
// returns it as the slot type. `lowerExpr` lowers a bare `int` key as
// `i32`. The codegen then:
//
//   %slot = alloca i32
//   store i32 %k, ptr %slot
//   call bz_dict_set_gen(ptr %recv, ptr %slot, ptr %vPtr)
//
// — and the runtime's `bz_dict_set_gen` does `memcpy(kc, key, d->key_size)`
// where `d->key_size` is the §8.6 box size for an Object element (16 B:
// `{ptr vtable, i64 rc, payload}`). With a 4-byte i32 slot, the runtime reads
// 12 B of stack redzone past the slot — MEASURED under ASan as
// "stack-buffer-overflow on address … partially overflows this variable" in
// `bz$valkey$hash` at bz_dict_set_gen's hash dispatch (src/runtime/
// bezel_runtime_collections.cpp:925). A pre-boxed spelling
// (`var o: Object := 1; d[o] := v`) lowers `o` as a ptr to a §8.6 box, the
// slot type IS ptrTy(), and `key_size` reads align — the exact reason no
// earlier regression test caught this.
//
// THE FIX. Apply `Emitter::boxForTarget` (src/codegen/LlvmBackend_Layout.cpp,
// beside the #4619 membership-query twin) to the lowered key before the
// spill-to-slot step in `lowerCollectionIndexerSet` (with parallel fixes at
// `lowerCollectionIndexerGet` / `lowerMoveFromCollectionSlot`, which take
// the same key-side path), when `keyTy` is `Object` (or any other §8.6
// boxing target) and the source is a value type. `boxForTarget` is a no-op
// for a reference source (`var o: Object := 1;`) and for a non-Object key,
// so every already-correct shape is byte-identical — the control rows below
// are here to prove exactly that.
//
// `boxForTarget` mints a fresh §8.6 box (24 B / 1 allocation for an int
// payload) and returns the box pointer; the runtime's `memcpy` copies the
// 16 B box bytes into the table storage, then `free`s its own copy. Our
// original box allocation is no longer referenced by anything — that 24 B per
// call is the leak the fix leaves behind, MEASURED and DECLINED here exactly
// as #4619 declined the membership-query residual (the same trade, the same
// already-accepted pre-boxed spelling carries a 24 B residual of its own;
// the way out is the runtime box-vtable guard #4619 also names).
//
// ORACLES. Each value-correctness row asserts a VALUE — the indexer reads
// back what was written, with the key being a literal then a variable, then
// the present/absent control, then the pre-boxed `Object` control. The
// loop-amplified row surfaces the leak profile (a 7,000 B floor over 300
// iterations against the 0 B the pre-fix crash produces — `leaksAtLeast`
// returns false on a non-leak sanitizer finding, which is what FAIL-WITHOUT
// requires).
//
// §8.6 / stdlib §4.8 (Dictionary<K, V>).

#include <regex>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// leaksAtLeast — a measured, margined leak-magnitude floor oracle (§30:
// NEVER pin an exact leak byte count — assert a FLOOR comfortably below the
// observed minimum). The pre-fix behaviour for #4640 is a
// stack-buffer-overflow (a CRASH, not a leak summary); the post-fix
// behaviour is one §8.6 box per indexer-set call, each ~24 B. 300 iterations
// × 1 box × 24 B = 7,200 B floor; measured 2026-09-23 on this lane, the
// loop leaked 7,200 B / 300 allocations — a 7,000 B floor is comfortably
// below. The pre-fix program does NOT emit a leak summary (it crashes), so
// `leaksAtLeast` returns false on that side, which is what FAIL-WITHOUT
// requires: the row pins the residual's presence, not its exact size.
bool leaksAtLeast(const std::string& program, long minBytes) {
    auto r = runAotChecked(program);
    if (r.ran) return false;  // no sanitizer finding at all -- wrongly clean
    static const std::regex kLeak(
        R"(AddressSanitizer: (\d+) byte\(s\) leaked)");
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::smatch m;
        if (std::regex_search(d.message, m, kLeak))
            return std::stol(m[1].str()) >= minBytes;
    }
    return false;  // sanitizer fired but not with a leak SUMMARY (e.g. a
                    // crash/UAF instead) -- correctly fails this row too
}
}  // namespace

// ── The issue's own repro: a bare `int` key indexer-set ─────────────────────

// `Dictionary<Object, int>[1] := 42` — pre-fix the codegen stores the i32
// literal to a 4-byte slot and passes its pointer to `bz_dict_set_gen`, which
// reads 16 B of stack redzone past the slot in `bz$valkey$hash`. ASan:
// stack-buffer-overflow. The same path was structurally broken for every
// value-typed key against an `Object`-keyed dictionary.
BEZEL_TEST(dict_object_bare_value_key_indexer_set_4640,
           dict_object_bare_int_key_indexer_set_no_crash_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: Dictionary<Object, int> := []; "
        "  d[1] := 42; "
        "  if d[1] = 42 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// The same shape through a VARIABLE rather than a literal, ruling out a
// constant-folding explanation. Pre-fix this ALSO overflowed — every bare
// value key took the broken spill path regardless of provenance.
BEZEL_TEST(dict_object_bare_value_key_indexer_set_4640,
           dict_object_bare_int_variable_key_indexer_set_no_crash_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: Dictionary<Object, int> := []; "
        "  var k: int := 7; "
        "  var v: int := 100; "
        "  d[k] := v; "
        "  if d[k] = 100 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// The present-then-absent key control. Pre-fix this BOTH overflowed on the
// `d[k]` read AND returned the wrong answer on the absent `d[99]`
// (it crashed instead of throwing KeyNotFoundException). With the fix the
// read returns the stored value and the absent read raises, exactly as
// expected per stdlib §4.8 — this row asserts two present reads answer
// correctly, not a throw-and-recover (the throw is an out-of-band effect
// `runAot` cannot inspect at this layer).
BEZEL_TEST(dict_object_bare_value_key_indexer_set_4640,
           dict_object_bare_int_key_present_control_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: Dictionary<Object, int> := []; "
        "  d[1] := 11; "
        "  d[2] := 22; "
        "  if d[1] = 11 and d[2] = 22 then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// The pre-boxed `Object` query/key control: `var o: Object := 1; d[o] := v`
// was already correct (the lowered `o` IS a ptr to a §8.6 box, the slot type
// IS ptrTy(), `key_size` reads align). The fix must leave this row
// byte-identical — `boxForTarget` is a no-op on an already-reference source.
// NOTE: the dict value type here is `Object` so that the pre-boxed `var v`
// can be assigned to a slot whose declared type matches. The READ oracle is
// a present/absent structural check (no value-equality compare against an
// int literal — that goes through the `emitRemoveElementEquality` funnel
// which is a separate lane's territory). We assert that the PRESENT key is
// found (Count = 1, Contains returns true) and the absent key throws
// KeyNotFoundException — both pure runtime-state observables.
BEZEL_TEST(dict_object_bare_value_key_indexer_set_4640,
           dict_object_preboxed_object_key_indexer_set_control_spec_8_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: Dictionary<Object, Object> := []; "
        "  var k: Object := 1; "
        "  var v: Object := 99; "
        "  d[k] := v; "
        "  if d.Count = 1 and d.Contains(k) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ── Leak profile: loop-amplified to surface the bounded residual ────────────
//
// Per #4619's discipline: NEVER pin an exact leak byte count — assert a FLOOR
// comfortably below the observed minimum. The pre-fix behaviour is a
// stack-buffer-overflow (a crash, not a leak summary); the post-fix behaviour
// is one §8.6 box per indexer-set call, each 24 B (16 B box + malloc header).
// 300 iterations × 1 box × 24 B = 7,200 B floor; measured 2026-09-23 on this
// lane, the loop leaked 7,200 B / 300 allocations — a 7,000 B floor is
// comfortably below. The pre-fix program DOES NOT LEAK (it crashes) so
// `leaksAtLeast` returns false on that side, which is what FAIL-WITHOUT
// requires: the row pins the residual's presence, not its exact size.

BEZEL_TEST(dict_object_bare_value_key_indexer_set_4640,
           dict_object_bare_int_key_indexer_set_leak_floor_spec_8_6) {
    BEZEL_EXPECT_TRUE(leaksAtLeast(
        "module M; class C public static method Main(): int "
        "  var i := 0; "
        "  var acc := 0; "
        "  while i < 300 do "
        "    var d: Dictionary<Object, int> := []; "
        "    d[i] := i * 2; "
        "    acc := acc + d[i]; "
        "    i := i + 1; "
        "  end; "
        "  return acc mod 256; "
        "end Main; end C;", 7000));
}