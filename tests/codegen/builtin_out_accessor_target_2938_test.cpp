// Spec-derived regression tests for issue #2938 — spec §22.3 "`ref` and `out`
// argument locations" applied to the BUILTIN `out` dispatch sites (stdlib
// §9.1 `TryParse`, §2.3 `Convert.TryTo*`, §2.5/§2.6 `DateTime`/`DateOnly`/
// `TimeOnly` `TryParse`, §2.4 `Guid.TryParse`, §4.8 `Dictionary.TryGetValue`,
// …), v0.240.
//
// THE DEFECT. The §22.3 list of assignable storage locations admits "a mutable
// local variable, a writable field, **an indexer slot**, or **a property with
// an accessible setter**" — and the very next paragraph fixes the latter two
// categories' semantics: "An **indexer slot or property** target is instead
// **copy-in / copy-out** under §17.6's evaluate-once discipline … on **normal
// return** the setter is called once with the slot's final value."
//
// The user-method call path implements that bracket (#1302:
// `emitAccessorRefArgCopyIn` / `emitAccessorRefArgWriteBacks`), as do the
// imported-method path (#2462), the ctor-chain path (#2579), and the element-
// policy path (Setup.cpp). The BUILTIN dispatch — ~25 sites that hand-roll
// their out-target address resolution — was never widened. Every such site did
//
//     llvm::Value* outAddr = lvalueAddr(*call.args[N].value, outTy);
//     if (!outAddr) return nullptr;
//
// `lvalueAddr` only resolves direct-alias locations (local / field / static /
// inline-array element); a property or indexer slot returns null. The call
// then bailed with an internal #2434 ("lowerLocalDecl's initializer expression
// failed to lower to a value for this local's slot") — a loud error, but the
// program compiles clean on the same code with a LOCAL out target and on the
// same code with a USER-METHOD callee, so the inconsistency is the bug.
//
// Each test below therefore fails without the fix: either on the VALUE (the
// dropped write-back, an internal #2434) or, in some negative controls, by
// correctly admitting a shape that was rejected before. Each test asserts the
// correct VALUE per the issue's requirement. The shape mirrors the
// #1302 accessor-writeback tests: fields declared in a KNOWN order with the
// property's backing field NOT first, so a wrong-slot write would disturb a
// neighbour as well as miss the target.

#include "support/aot_support.hpp"   // runAot — the real bezelc AOT oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// =============================================================================
// T1 — #2938's own repro: `int.TryParse(out b.IP)` where `b.IP` is a block-form
// property. The local-form control (`out var n`) is correct on HEAD (v=42).
// The property form reports #2434 internally and the write is dropped (v=0).
// Post-fix: both spellings land 42 in `b._v`. Fields `_z` and `_v` are
// declared in that order so `_z` is the property's "would-be neighbour" —
// a wrong-slot write would bump `_z` to 42 instead. Correct: 0 + 42 + 42 + 0
// = 84.
// =============================================================================
BEZEL_TEST(builtin_out_accessor_target_2938, tryparse_to_block_property_writes_back) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Box "
        "  public _z: int := 0; "
        "  public _v: int := 0; "
        "  public property IP: int "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end IP; "
        "end Box; "
        "public class MMain "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var b := new Box(); "
        "    var ok := int.TryParse(\"42\", out b.IP); "
        "    if ok then acc := acc + 1; end; "
        "    if b._v = 42 then acc := acc + 2; end; "
        "    if b._z = 0 then acc := acc + 4; end; "
        "    return acc; "
        "  end Main; "
        "end MMain;"), 7);
}

// =============================================================================
// T2 — the same defect on the integer tower's narrow widths: `short.TryParse`,
// `byte.TryParse`, etc. (§9.1). The bracket's slot is sized to the LOCATION's
// type, so the truncated value the builtin writes matches the property's
// declared type. A wrong-slot write would either truncate the wrong neighbour
// or fail to round-trip at all; the test pins the success.
// =============================================================================
BEZEL_TEST(builtin_out_accessor_target_2938, tryparse_short_to_property_writes_back) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Box "
        "  public _z: int := 0; "
        "  public _v: short := 0; "
        "  public property IP: short "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end IP; "
        "end Box; "
        "public class MMain "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    var ok := short.TryParse(\"1000\", out b.IP); "
        "    if ok and b._v = 1000 and b._z = 0 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end MMain;"), 1);
}

// =============================================================================
// T3 — `Dictionary<string, int>.TryGetValue(out b.IP)` — stdlib §4.8 / §22.3
// (the title's "TryGetValue" exemplar). The dictionary's value type is `int?`
// (stdlib §1 / v0.171 / #1336: every extraction Try's out parameter is `T?`
// and the false path assigns `null`); the property's type is therefore
// `int?` so the bracket round-trips a 4-byte slot. A wrong-slot write would
// clobber `_z` (field 0) — we observe it as 0 to confirm.
// =============================================================================
BEZEL_TEST(builtin_out_accessor_target_2938, trygetvalue_to_block_property_writes_back) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Box "
        "  public _z: int := 0; "
        "  public _v: int := 0; "
        "  public property IP: int? "
        "    get => _v; "
        "    set => do _v := value ?? 0; end; "
        "  end IP; "
        "end Box; "
        "public class MMain "
        "  public static method Main(): int "
        "    using var d := new Dictionary<string, int>(); "
        "    d[\"k\"] := 7; "
        "    using var b := new Box(); "
        "    var ok := d.TryGetValue(\"k\", out b.IP); "
        "    if ok and b._v = 7 and b._z = 0 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end MMain;"), 1);
}

// =============================================================================
// T4 — `Convert.TryToInt32(s, out b.IP)` (§2.3 / stdlib). The `Convert` class
// is a different builtin dispatch site than the primitive tower's own
// `int.TryParse`; both routes converge on the same `lvalueAddr` defect.
// =============================================================================
BEZEL_TEST(builtin_out_accessor_target_2938, convert_trytoint32_to_property_writes_back) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Box "
        "  public _z: int := 0; "
        "  public _v: int := 0; "
        "  public property IP: int "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end IP; "
        "end Box; "
        "public class MMain "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    var ok := Convert.TryToInt32(\"42\", out b.IP); "
        "    if ok and b._v = 42 and b._z = 0 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end MMain;"), 1);
}

// =============================================================================
// T5 — `Guid.TryParse(s, out b.IP)` (§2.4). A non-trivial value type — the
// bracket round-trips a 16-byte Guid through the slot. A wrong-slot write
// would clobber `_z` (field 0) with the first 4 bytes of the Guid bytes,
// and `_v` (field 1, the property's backing) with bytes 4..7; we observe
// both as 0 to confirm.
// =============================================================================
BEZEL_TEST(builtin_out_accessor_target_2938, tryparse_guid_to_property_writes_back) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Box "
        "  public _z: int := 0; "
        "  public _v: Guid := default(Guid); "
        "  public property IP: Guid "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end IP; "
        "end Box; "
        "public class MMain "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    var ok := Guid.TryParse("
        "        \"12345678-1234-1234-1234-123456789012\", out b.IP); "
        "    if not ok then return 0; end; "
        "    var s := b.IP.ToString(); "
        "    if s = \"12345678-1234-1234-1234-123456789012\" "
        "       and b._z = 0 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end MMain;"), 1);
}

// =============================================================================
// T6 — a USER-DECLARED INDEXER slot (`b[i]`) as the `out` target of an
// `int.TryParse` call. The receiver evaluates once and the index evaluates
// once — same evaluate-once discipline §22.3 mandates for the user-method
// path; the bracket extends it to the builtin dispatch.
// =============================================================================
BEZEL_TEST(builtin_out_accessor_target_2938, tryparse_to_user_indexer_writes_back) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Box "
        "  private _v: int := 0; "
        "  public constructor() this._v := 0; end constructor; "
        "  public property [i: int]: int "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "end Box; "
        "public class MMain "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    var ok := int.TryParse(\"42\", out b[0]); "
        "    if ok and b[0] = 42 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end MMain;"), 1);
}

// =============================================================================
// T7 — CONTROL. The local-form `out var n` already works on HEAD (T1's
// `+8` bit). The field-form `out b._v` already works on HEAD (the
// `lvalueAddr` resolves a true-alias field). The bracket must NOT swallow
// these — they go through the alias path, not the bracket. Pin the
// pre-existing correctness so a future refactor cannot regress it.
// =============================================================================
BEZEL_TEST(builtin_out_accessor_target_2938, local_out_target_still_works_via_alias) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class MMain "
        "  public static method Main(): int "
        "    var n: int := -1; "
        "    var ok := int.TryParse(\"42\", out n); "
        "    if ok and n = 42 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end MMain;"), 1);
}

BEZEL_TEST(builtin_out_accessor_target_2938, field_out_target_still_works_via_alias) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class Box "
        "  public _v: int := 0; "
        "end Box; "
        "public class MMain "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    var ok := int.TryParse(\"42\", out b._v); "
        "    if ok and b._v = 42 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end MMain;"), 1);
}
