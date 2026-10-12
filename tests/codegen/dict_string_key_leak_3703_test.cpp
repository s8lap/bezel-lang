// Codegen — fix #3703 / stdlib §4.8, §4.9 / spec §28.9 (v0.293):
// a borrowing `Dictionary<string, V>` (and `OwningDictionary<string, V>`) never
// released the +1 a fresh-producer string KEY expression carries.
//
// Authority. stdlib §4.9 (`OwningDictionary<K, V>`, v0.293) states the
// ownership fact directly: "Owns its values; keys are value-like (typically
// `string`, `int`, or a record with value equality)." The compiler's own note
// at src/codegen/LlvmBackend_Setup.cpp:18163 records the consequence — keys
// are never disposed via `dictValueDisposer` — and cites §4.9 for it.
// (An earlier revision of this header attributed that wording to §4.8 and
// presented the C++ comment as though it were stdlib prose; both were wrong.)
// So the dictionary's own scope-end free does not own its keys. The runtime backs this precisely:
// `bz_dict_set_str` (src/runtime/bezel_runtime_collections.cpp) copies a
// string key's BYTES via `strndup` into its own malloc'd buffer — it never
// retains the caller's `bz_string*`. That is correct and sufficient for a
// BORROWED key (a local variable, a literal, a field read) because the
// caller's own existing owner keeps discharging it as before.
//
// It is NOT sufficient for a key expression that is itself a fresh +1
// PRODUCER (spec §28.9's runtime-managed-value discipline; see #429's
// analogous VALUE-side retain) — `$"...{i}"` string interpolation, string
// concatenation, or a case/if arm-meet. Nothing else ever reads or discharges
// that producer's own reference once the dictionary has copied its bytes, so
// every one of these call sites leaked one `bz_string` header + one data
// buffer per call, for every KEY argument that happened to be a fresh
// producer:
//
//   * the indexer `d[key] := v` (both the disposing and non-disposing forms)
//   * the indexer get `d[key]` and `move d[key]` (BOTH the found-key path
//     AND the absent-key `KeyNotFoundException` throw path)
//   * `.Add(key, v)` / `.Set(key, v)` (BOTH the insert path AND the
//     duplicate-key `ArgumentException` throw path, for both the borrowing
//     and OwningDictionary sub-arms)
//   * `.TryAdd(key, v)`, `.Remove(key)`
//   * `.TryGetValue(key, out v)`, `.GetOrNull(key)`, `.GetOrAdd(key, factory)`
//   * `.PopValue(key)`, `.TryPopValue(key, out v)`
//   * `key in dict` (§4.8's `ContainsKey` alias)
//   * a dictionary literal `[key: v, ...]` with a built key
//
// `Set<string>` is UNAFFECTED — a dict-backed Set stores its string elements
// as the table's KEY itself (a pointer copy, `dictStringKeys` /
// `bz_set_string_keys_release`), a completely different representation from
// `Dictionary<string, V>`'s byte-copied string-mode key. This fix does not
// touch that path.
//
// The fix: at every site above, release a fresh +1 KEY producer
// (`isPlusOneStringProducer` — the same predicate the VALUE side already used
// for #429) once its buffer's last read is complete. A BORROWED key must
// never be released here (that would be releasing a reference this call did
// not mint — a premature free of a string the caller still owns) —
// `reused_key_local_not_prematurely_released_28_9` below is the soundness
// check for that half of the fix.
//
// THROW-PATH SITES (two, both closed in this revision, not filed as
// residuals): a call whose success path releases the key only after a SECOND
// read of the key's buffer (a duplicate/absent-key probe, then — only on the
// non-throwing edge — the real insert/take) leaks on the OTHER edge if that
// edge diverges (throws) before ever reaching the success-path release:
//   * `.Add(key, v)` on an ALREADY-PRESENT key (`ArgumentException`) — both
//     the borrowing and the OwningDictionary sub-arm independently probe,
//     branch, and (only on the OwningDictionary arm) dispose the rejected
//     VALUE before throwing; neither arm's throw edge released the KEY.
//   * `move d[key]` on an ABSENT key (`KeyNotFoundException`).
// Both are fixed the same way: replace the plain `emitGuardThrow` with a
// manual branch (mirroring the OwningDictionary `.Add` arm's pre-existing
// dispose-then-throw pattern for the VALUE) that releases a fresh +1 KEY
// producer on the throw edge, before throwing — provably single-execution,
// because the throw edge and the (pre-existing) success-path release are
// two arms of one conditional branch, never both taken. See the throw-path
// tests below (`add_duplicate_key_*` and `move_absent_key_*`), each looped
// 200x.
//
// Independently re-verified by review (not just asserted here): every
// string-key runtime entry point (`bz_dict_alloc`, `bz_dict_set_str`,
// `bz_dict_try_get_str`, `bz_dict_contains_str`, `bz_dict_remove_str[_disposing]`,
// `bz_dict_pop_value_str`, `dict_find` — bezel_runtime_collections.cpp:585-931)
// reads only `(k, k_len)` bytes and never retains the caller's `bz_string*`;
// `Set<string>`'s genuinely different pointer-key representation
// (bezel_runtime_collections.cpp:1416-1476) confirms the issue's own
// `dictStringKeys` theory does not apply here. `isPlusOneStringProducer`
// (93 call sites) never fires on a bare identifier, member access, or
// literal, so a borrowed key is provably never released by this fix. Every
// multi-read site was re-traced by hand through the real
// `CreateCondBr`/`SetInsertPoint`/PHI wiring: `TryAdd` and `GetOrAdd` release
// at the true CFG join (`endBB`); the indexer-set's branches are compile-time
// `if`/`else`, so the trailing release is unconditional. No UAF, no
// double-free, at any release site added by this fix.
//
// Every leak-sanitizer row is loop-amplified (200x): a one-shot LSan run can
// pass on both sides of a fix because the leaked allocation is still
// stack-reachable at process exit (LANE_RULES §3).
//
// Two SEPARATE, PRE-EXISTING defects surfaced while writing these tests —
// both reproduce identically on the unmodified base and neither is caused by
// or fixed by this change:
//   * #3871 — `OwningDictionary<K,V>.GetOrAdd`'s factory closure leaks its
//     environment cell when the factory lambda CAPTURES an outer variable
//     (independent of the key's shape; a non-capturing factory is clean).
//   * #3872 — a named string LOCAL's own scope-end release is lost
//     specifically when that local is passed as a dict indexer-set KEY
//     argument (a BORROWED read elsewhere releases correctly, per the bare
//     `var s := $"..."; s.Length;` control — the loss is keyed on the
//     indexer-set use).
// Both are filed with measured 200x repros; the tests below route around
// them (a non-capturing factory; `runAotCheckedLenient` for the one test
// that must reuse a named key local) rather than fold them into this fix's
// blast radius.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

std::string prog(const std::string& mainBody) {
    return "module M; class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

// A resource class for the OwningDictionary-side tests: a static counter
// tracks live-vs-disposed and a Tag field carries a per-instance value.
std::string progR(const std::string& mainBody) {
    return
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  public Tag: int := 0; "
        "  public constructor(t: int) Tag := t; end constructor; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// stdlib §4.8 / spec §28.9: the exact repro from issue #3703 — indexer SET
// and GET with a built (interpolated) string key, `using var` Dictionary.
// FAIL-WITHOUT the fix: LSan reports ~14780 B / 800 allocations at 200x
// (400 built keys, 2 allocations each: the bz_string header + its data
// buffer). PASS-WITH: clean.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_string_key_leak_3703, indexer_set_and_get_built_key_leak_free_4_8) {
    // total = 200 * 30 = 6000, over the 8-bit exit-code ceiling — assert on
    // captured stdout instead (LANE_RULES: an observable > 255 must move to
    // stdoutText).
    JitResult r = runAotChecked(prog(
        "var total := 0; var i := 0; "
        "while i < 200 do "
        "  using var d := new Dictionary<string, int>(); "
        "  d[$\"a{i}\"] := 10; "
        "  d[$\"bb{i}\"] := 20; "
        "  total := total + d[$\"a{i}\"] + d[$\"bb{i}\"]; "
        "  i := i + 1; "
        "end; "
        "Console.WriteLine($\"total={total}\"); "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "total=" + std::to_string(200 * (10 + 20)) + "\n");
}

// ---------------------------------------------------------------------------
// stdlib §4.8: `.Add(key, v)` on a fresh (non-duplicate) built key each
// iteration, and `.TryAdd(key, v)` exercising BOTH edges (fresh insert, then
// a duplicate probe on the SAME built key) — the join-point shape where the
// key's buffer is read once for the duplicate probe and, only on the insert
// edge, a second time for the actual insert; the release must sit after
// BOTH edges have joined, not after the first read (else the insert edge
// would use-after-free the just-released key).
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_string_key_leak_3703, add_and_tryadd_both_edges_built_key_leak_free_4_8) {
    JitResult r = runAotChecked(prog(
        "var okAdd := 0; var okInsert := 0; var okDup := 0; var i := 0; "
        "while i < 200 do "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Add($\"a{i}\", i); "
        "  if d[$\"a{i}\"] = i then okAdd := okAdd + 1; end; "
        "  if d.TryAdd($\"b{i}\", i) then okInsert := okInsert + 1; end; "
        "  if not d.TryAdd($\"b{i}\", 999) then okDup := okDup + 1; end; "
        "  i := i + 1; "
        "end; "
        "return int(okAdd = 200) * 4 + int(okInsert = 200) * 2 + "
        "       int(okDup = 200); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ---------------------------------------------------------------------------
// stdlib §4.8: `.ContainsKey(key)`, `.Remove(key)` and `key in dict` — three
// call sites that use a built key ONLY to probe/unlink, never to store it.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_string_key_leak_3703, contains_remove_and_in_operator_built_key_leak_free_4_8) {
    JitResult r = runAotChecked(prog(
        "var hits := 0; var i := 0; "
        "while i < 200 do "
        "  using var d := new Dictionary<string, int>(); "
        "  d[\"seed\"] := 1; "
        "  if d.ContainsKey($\"x{i}\") then hits := hits + 1; end; "
        "  if $\"y{i}\" in d then hits := hits + 1; end; "
        "  d[$\"z{i}\"] := 5; "
        "  if d.Remove($\"z{i}\") then hits := hits + 1; end; "
        "  i := i + 1; "
        "end; "
        // ContainsKey($"x{i}") and $"y{i}" in d correctly MISS (only "seed" and,
        // later, "z{i}" are ever present); Remove($"z{i}") correctly HITS. So
        // hits must land at exactly 200 (one per iteration, from Remove alone).
        "return int(hits <> 200); "));  // 0 on the expected outcome
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// stdlib §4.8: `.TryGetValue(key, out v)` and `.GetOrNull(key)` with a built
// key on both the hit and miss edges.
// ---------------------------------------------------------------------------
// The sum below (2 * (0+1+...+199) = 39800) exceeds the 8-bit exit-code
// ceiling, so it is asserted on captured stdout instead (LANE_RULES: an
// observable > 255 must move to stdoutText; runAotChecked's JitResult carries
// the same stdoutText field as runAot).
BEZEL_TEST(dict_string_key_leak_3703, trygetvalue_and_getornull_built_key_sum_and_leak_free_4_8) {
    JitResult r = runAotChecked(prog(
        "var sum := 0; var i := 0; "
        "while i < 200 do "
        "  using var d := new Dictionary<string, int>(); "
        "  d[$\"a{i}\"] := i; "
        "  var v: int? := null; "
        "  if d.TryGetValue($\"a{i}\", out v) then sum := sum + v!; end; "
        "  if d.TryGetValue($\"missing{i}\", out v) then sum := sum + 999; end; "
        "  var gv := d.GetOrNull($\"a{i}\"); "
        "  if gv <> null then sum := sum + gv; end; "
        "  var gv2 := d.GetOrNull($\"missing{i}\"); "
        "  if gv2 <> null then sum := sum + 999; end; "
        "  i := i + 1; "
        "end; "
        "Console.WriteLine($\"sum={sum}\"); "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    int expected = 0;
    for (int i = 0; i < 200; ++i) expected += 2 * i;  // TryGetValue hit + GetOrNull hit
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "sum=" + std::to_string(expected) + "\n");
}

// ---------------------------------------------------------------------------
// stdlib §4.9: `OwningDictionary<string, R>.GetOrAdd(key, factory)` — the
// join-point shape where the built key's data/len are cached ONCE and reused
// on BOTH the hit probe and, only on the miss edge, the insert + re-read; the
// release must sit after the join. MISS invokes the factory (an owned `R`,
// stored into the dict — 1 per iteration); HIT does not.
//
// The factory lambdas below deliberately capture NOTHING from the enclosing
// scope (they compute R's tag from their own `k` parameter or a constant) —
// a SEPARATE, pre-existing defect (a capturing GetOrAdd factory leaks its
// closure's environment cell; measured independent of this fix and filed as
// #3871) would otherwise mask this test's own signal.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_string_key_leak_3703, getoradd_owning_built_key_leak_free_4_9) {
    JitResult r = runAotChecked(progR(
        "var i := 0; "
        "while i < 200 do "
        "  using var d := new OwningDictionary<string, R>(); "
        "  var v1 := d.GetOrAdd($\"k{i}\", lambda(k: string) => move (new R(k.Length))); "
        "  var v2 := d.GetOrAdd($\"k{i}\", lambda(k: string) => move (new R(0))); "
        "  if v1.Tag <> v2.Tag then return 1; end; "  // HIT: v2 is the SAME stored R
        "  i := i + 1; "
        "end; "
        "return R._n; "));  // every MISS-path R is disposed at scope end
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// ---------------------------------------------------------------------------
// stdlib §4.9: `OwningDictionary<string, R>.PopValue(key)` and
// `.TryPopValue(key, out v)` with a built key — single-read shapes (the key's
// buffer is consumed once, before the found/not-found branch).
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_string_key_leak_3703, popvalue_and_trypopvalue_owning_built_key_leak_free_4_9) {
    JitResult r = runAotChecked(progR(
        "var sum := 0; var i := 0; "
        "while i < 200 do "
        "  using var d := new OwningDictionary<string, R>(); "
        "  d[$\"x{i}\"] := new R(i); "
        "  using var popped := d.PopValue($\"x{i}\"); "
        "  sum := sum + popped.Tag; "
        "  d[$\"y{i}\"] := new R(i + 1); "
        "  var got: R? := null; "
        "  var hit := d.TryPopValue($\"y{i}\", out got); "
        "  if hit then sum := sum + got!.Tag; end; "
        "  got?.Dispose(); "
        "  i := i + 1; "
        "end; "
        "Console.WriteLine($\"sum={sum}\"); "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    int expected = 0;
    for (int i = 0; i < 200; ++i) expected += i + (i + 1);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "sum=" + std::to_string(expected) + "\n");
}

// ---------------------------------------------------------------------------
// stdlib §4.8 / §15.4: a dictionary literal `[key: v]` whose KEY is a built
// (interpolated) expression, not a literal — the third independent site
// (materializeHeapDictLiteral) that copies the key's bytes without ever
// retaining the bz_string* the program handed in.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_string_key_leak_3703, dict_literal_built_key_leak_free_15_4) {
    JitResult r = runAotChecked(prog(
        "var total := 0; var m := 0; "
        "while m < 200 do "
        "  var lit: Dictionary<string, int> := [$\"lit{m}\": m]; "
        "  total := total + lit[$\"lit{m}\"]; "
        "  m := m + 1; "
        "end; "
        "return int(total = 199 * 200 / 2); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ---------------------------------------------------------------------------
// SOUNDNESS: a key that is a fresh +1 producer AT ITS OWN BINDING but is then
// read via a BORROWING local variable — `isPlusOneStringProducer` looks at
// the KEY ARGUMENT EXPRESSION at each call site (an IdentifierExpr naming the
// local), which is never itself a producer, so the local's own +1 is never
// released by THIS fix; only the local's own scope-end path may do that. If
// the fix mis-fired on a borrowed read here, this test would read corrupted
// string content (freed-and-reused buffer) or crash/UAF under ASan.
//
// Uses `runAotCheckedLenient` (generated-code ASan active — UAF/OOB in an
// emitted load still trips — but `detect_leaks=0`), NOT `runAotChecked`: a
// SEPARATE, pre-existing defect (measured independent of this fix, filed as
// #3872) leaks a named string LOCAL's own scope-end release specifically
// when that local is passed as a dict indexer-set KEY argument (reproduces
// identically on the unmodified base — this fix neither causes nor touches
// it). That is a real leak, but not the one under test here; a strict
// `runAotChecked` on this exact shape would conflate the two.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_string_key_leak_3703, reused_key_local_not_prematurely_released_28_9) {
    JitResult r = runAotCheckedLenient(prog(
        "var total := 0; var j := 0; "
        "while j < 200 do "
        "  using var d := new Dictionary<string, int>(); "
        "  var key := $\"shared{j}\"; "  // fresh producer, bound to a local
        "  d[key] := 1; "
        "  if d.ContainsKey(key) then total := total + 1; end; "
        "  if key in d then total := total + 1; end; "
        "  var v: int? := null; "
        "  if d.TryGetValue(key, out v) then total := total + v!; end; "
        "  total := total + key.Length; "  // key must still be a live, correct string
        "  j := j + 1; "
        "end; "
        "Console.WriteLine($\"total={total}\"); "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    // "shared" + digits(j): 7 chars for j in [0,9], 8 for [10,99], 9 for [100,199].
    int expected = 0;
    for (int j = 0; j < 200; ++j) {
        int digits = (j < 10) ? 1 : (j < 100 ? 2 : 3);
        expected += 1 /* ContainsKey */ + 1 /* in */ + 1 /* TryGetValue value */ +
                    (6 + digits) /* "shared" + digits(j) */;
    }
    BEZEL_EXPECT_EQ(r.stdoutText, "total=" + std::to_string(expected) + "\n");
}

// ---------------------------------------------------------------------------
// stdlib §4.9 / spec §28.11: `move d[key]` on an OwningDictionary<string, R>,
// FOUND-key path, with a built (interpolated) key — the release
// `lowerMoveFromCollectionSlot` installs after the null-store. Before this
// commit this exact release existed but had NO test anywhere in the suite:
// every pre-existing `move d[...]` test (tests/codegen/move_collection_slot_
// test.cpp) uses a LITERAL key, on which `isPlusOneStringProducer` never
// fires, so the added code was reachable only by inspection, not by any test.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_string_key_leak_3703, move_found_key_built_key_leak_free_4_9) {
    JitResult r = runAotChecked(progR(
        "var sum := 0; var i := 0; "
        "while i < 200 do "
        "  using var d := new OwningDictionary<string, R>(); "
        "  d[$\"m{i}\"] := move (new R(i)); "
        "  using var taken := move d[$\"m{i}\"]; "
        "  sum := sum + taken.Tag; "
        "  i := i + 1; "
        "end; "
        "Console.WriteLine($\"sum={sum}\"); "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.ran);
    int expected = 0;
    for (int i = 0; i < 200; ++i) expected += i;
    BEZEL_EXPECT_EQ(r.stdoutText, "sum=" + std::to_string(expected) + "\n");
}

// ---------------------------------------------------------------------------
// stdlib §4.9 / spec §28.11: `move d[key]` on an OwningDictionary<string, R>,
// ABSENT-key path (raises `KeyNotFoundException`), with a built key — the
// THROW-PATH twin this fix closes (review send-back): the found-key release
// above never runs when the key is absent (`emitGuardThrow` used to diverge
// unconditionally before any release point); the manual branch this fix adds
// releases the key on the throw edge instead, before throwing.
//
// FAIL-WITHOUT this specific edge (measured by review before the fix landed
// here): 7290 B / 400 allocations leaked at 200x (the try-get probe's kd/kl
// read a key whose own reference was never discharged on this edge).
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_string_key_leak_3703, move_absent_key_throw_path_built_key_leak_free_4_9) {
    JitResult r = runAotChecked(progR(
        "var caught := 0; var i := 0; "
        "while i < 200 do "
        "  using var d := new OwningDictionary<string, R>(); "
        "  d[$\"present{i}\"] := move (new R(i)); "
        "  try "
        "    using var taken := move d[$\"absent{i}\"]; "
        "  catch KeyNotFoundException => caught := caught + 1; "
        "  end; "
        "  i := i + 1; "
        "end; "
        "return caught; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 200);
}

// ---------------------------------------------------------------------------
// stdlib §4.8: `.Add(key, v)` on an ALREADY-PRESENT built key (borrowing
// Dictionary<string, int>) — raises `ArgumentException`. THROW-PATH twin
// this fix closes alongside the `move d[key]` one above: the real-insert's
// release (this fix, further down the SAME arm) never runs on the duplicate
// edge; the manual branch this fix adds releases the key before throwing.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_string_key_leak_3703, add_duplicate_key_throw_path_borrowing_built_key_leak_free_4_8) {
    JitResult r = runAotChecked(prog(
        "var caught := 0; var i := 0; "
        "while i < 200 do "
        "  using var d := new Dictionary<string, int>(); "
        "  d.Add($\"dup{i}\", 1); "
        "  try "
        "    d.Add($\"dup{i}\", 2); "
        "  catch ArgumentException => caught := caught + 1; "
        "  end; "
        "  i := i + 1; "
        "end; "
        "return int(caught = 200); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ---------------------------------------------------------------------------
// stdlib §4.9: the OwningDictionary<string, R> sibling of the test above —
// `.Add(key, v)` on an already-present built key disposes the rejected VALUE
// (pre-existing #1214 behaviour, unaffected) AND, per this fix, releases the
// KEY on the same throw edge.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_string_key_leak_3703, add_duplicate_key_throw_path_owning_built_key_leak_free_4_9) {
    JitResult r = runAotChecked(progR(
        "var caught := 0; var i := 0; "
        "while i < 200 do "
        "  using var d := new OwningDictionary<string, R>(); "
        "  d[$\"dup{i}\"] := move (new R(i)); "
        "  try "
        "    d.Add($\"dup{i}\", new R(999)); "
        "  catch ArgumentException => caught := caught + 1; "
        "  end; "
        "  i := i + 1; "
        "end; "
        "return int(caught = 200); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}
