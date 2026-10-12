// fix #891 / §28.11: OwningDictionary.TryPopValue left its out-owned parameter
// uninitialized when the key was absent — the runtime's bz_dict_pop_value_*
// function does not write *out on the not-found path, so the tmpSlot alloca
// held garbage; subsequent use of the out param (e.g. v?.Dispose()) then
// dereferenced the garbage pointer and crashed with SIGSEGV rc=139.
//
// Fix: pre-null tmpSlot (store ConstantPointerNull) before calling the runtime,
// so the not-found path loads null and any null-conditional use is a safe no-op.
// The same pre-null is applied to TryGetValue's tmpSlot (same structural risk).
//
// Test matrix:
//   (a) missing key, empty dict  → no crash, v is null, v?.Dispose() no-op
//   (b) missing key, non-empty dict → no crash, v is null, remaining entries
//       still disposed by dict at scope end (Cell._n→0, no double-free)
//   (c) present key              → true, v disposed exactly once (Cell._n→0)
//   (d) out using var, missing   → no crash, nothing disposed (Cell._n=0)
//   (e) out using var, present   → disposed once at scope end (Cell._n=0)
//   (f) sema gate (#860) still rejects dropped out-owned
//       (compile-error, not a JIT test — confirmed separately)
//   (g) borrowing TryGetValue(missing) regression — still works, out param=0
//
// Value tests — runJit returns the raw int (kept in 0..255).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// A class whose destructor decrements a static counter.  A correct program
// ends with Cell._n == 0 (all constructed objects have been destructed exactly
// once); a leak leaves _n > 0; a double-free drives _n negative (returned as
// rc ≠ 0 since rc is only the low 8 bits, but the fold reveals it).
const char* kCellHdr =
    "module M; class Cell public static _n: int := 0; "
    "  public constructor() Cell._n := Cell._n + 1; end constructor; "
    "  destructor Cell._n := Cell._n - 1; end destructor; end Cell; ";
}  // namespace

// (a) Missing key on an EMPTY dict: was rc=139 SIGSEGV before the fix.
//     TryPopValue returns false and leaves v null (v declared inline via
//     `out owned var`), v?.Dispose() is a safe null no-op, program returns 0.
//     §28.11, #891.
BEZEL_TEST(trypopvalue_missing_key_891, empty_dict_missing_key_no_crash) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  do using var d := new OwningDictionary<string, Cell>(); "
        "    var ok := d.TryPopValue(\"x\", out var v); "
        "    v?.Dispose(); "
        "    if ok then return 99; end; "
        "  end; return Cell._n; end Main; end C;"), 0);
}

// (b) Missing key, NON-EMPTY dict: v stays null, the other entries are still
//     owned by the dict and disposed at scope end → Cell._n returns to 0.
//     §28.11, #891.
BEZEL_TEST(trypopvalue_missing_key_891, nonempty_dict_missing_key_others_disposed) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  do using var d := new OwningDictionary<string, Cell>(); "
        "    d.Add(\"a\", move (new Cell())); "
        "    d.Add(\"b\", move (new Cell())); "
        "    var ok := d.TryPopValue(\"x\", out var v); "
        "    v?.Dispose(); "
        "    if ok then return 99; end; "
        "  end; return Cell._n; end Main; end C;"), 0);
}

// (c) PRESENT key: TryPopValue returns true, the element is transferred to v
//     (the dict no longer holds it), v?.Dispose() disposes it exactly once,
//     and the dict disposes nothing at scope end → Cell._n = 0.  §28.11, #891.
BEZEL_TEST(trypopvalue_missing_key_891, present_key_disposed_exactly_once) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  do using var d := new OwningDictionary<string, Cell>(); "
        "    d.Add(\"k\", move (new Cell())); "
        "    var ok := d.TryPopValue(\"k\", out var v); "
        "    v?.Dispose(); "
        "  end; return Cell._n; end Main; end C;"), 0);
}

// (d) `out using var` form, MISSING key: scope-end dynamic-dispose of v runs
//     but v is null → safe no-op, Cell._n stays 0.  §28.11, #891.
BEZEL_TEST(trypopvalue_missing_key_891, out_using_var_missing_no_crash) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  do using var d := new OwningDictionary<string, Cell>(); "
        "    var ok := d.TryPopValue(\"missing\", out using var v); "
        "  end; return Cell._n; end Main; end C;"), 0);
}

// (e) `out using var` form, PRESENT key: scope-end dynamic-dispose disposes
//     the transferred element exactly once → Cell._n = 0.  §28.11, #891.
BEZEL_TEST(trypopvalue_missing_key_891, out_using_var_present_disposed_once) {
    BEZEL_EXPECT_EQ(run(std::string(kCellHdr) +
        "class C public static method Main(): int "
        "  do using var d := new OwningDictionary<string, Cell>(); "
        "    d.Add(\"k\", move (new Cell())); "
        "    var ok := d.TryPopValue(\"k\", out using var v); "
        "  end; return Cell._n; end Main; end C;"), 0);
}

// (g) Regression — borrowing Dictionary<string,int>.TryGetValue(missing key):
//     tmpSlot is pre-nulled and, per v0.171 (#1336), the not-found path assigns
//     `null` to the now-`int?` out param (§22.3 — every out assigned on every
//     exit path) — no SIGSEGV, and the miss is observably null.  §28.11, #891.
BEZEL_TEST(trypopvalue_missing_key_891, trygetvalue_missing_key_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  do using var d := new Dictionary<string, int>(); "
        "    d.Add(\"a\", 42); "
        "    var v: int? := null; "  // v0.171 (#1336): out is V?
        "    var ok := d.TryGetValue(\"missing\", out v); "
        "    if ok then return 1; end; "
        "    if v = null then return 0; end; "  // miss assigns null
        "    return 5; "
        "  end; end Main; end C;"), 0);
}
