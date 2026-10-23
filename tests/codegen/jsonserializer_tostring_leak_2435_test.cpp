// Codegen / leak — `JsonSerializer.ToString()` leaked its result string.
// Issue #2435.
//
// Spec: stdlib §2.15 "Concrete serialization formats" (`JsonSerializer.method
// ToString(): string;` — "convenience: UTF-8 → string") / language spec §4.6
// `string` ("runtime-managed") / §28.9 "Runtime-managed types" (a producer
// hands the caller exactly +1, the consumer transfers-or-releases it once).
// Spec revision v0.235.
//
// THE BUG. A consumer of a string value (return / assign / bind) decides once,
// for the whole value, whether it is a +1 PRODUCER (transfer the reference) or
// a +0 BORROW (mint one via bz_string_retain). `JsonSerializer.ToString()`
// lowers (LlvmBackend_Serialization.cpp's `rcn == "JsonSerializer"` branch) to
// the runtime `bz_jser_to_string`, which decodes the accumulated JSON bytes
// into a freshly-allocated heap `bz.string` at rc := 1 (bz_string_init_rc) — a
// genuine +1 producer. But `isPlusOneStringProducer` (LlvmBackend_Setup.cpp)
// did not recognize this call shape, so `var json := s.ToString()` was treated
// as a BORROW and RETAINED at bind (rc 1->2), then released at scope end
// (rc 2->1) — the producer's original +1 was never discharged and the whole
// result leaked. LSan (pre-fix, this file's own repro): "49 byte(s) leaked in
// 2 allocation(s)" — a 32-byte direct leak (the `bz.string` header, `calloc`)
// and a 17-byte indirect leak (the decoded UTF-8 data buffer, `malloc`), both
// rooted at `bz_jser_to_string <- main` — matching #2435's own reported shape
// (87 bytes / 2 allocations on its larger Hub/Slot repro; the exact byte count
// scales with payload length, the allocation COUNT and root frame do not). The
// sibling `ToBytes()` + `using var`-disposed `OwningBuffer<byte>` path was
// already correct (`owned OwningBuffer<byte>`, discharged by `using var`) and
// stays unaffected by this fix.
//
// THE FIX. isPlusOneStringProducer recognizes a no-arg `ToString()` on a
// JsonSerializer receiver (CallExpr, empty args, member "ToString", a Named
// receiver type with NO user `declaration` and headName() == "JsonSerializer"
// — the exact synthetic-type shape tryLowerSerializationCall's `rcn` dispatch
// requires before it ever reaches the "JsonSerializer" branch, so a same-named
// user class — which WOULD carry a real `declaration` — cannot be
// misclassified) as a +1 producer, so the consumer TRANSFERS the single
// reference instead of minting a second. No double-free: no extra retain is
// emitted, and the runtime mints exactly one reference, which the scope-end
// release (when the escape walk proves the local non-escaping) drops exactly
// once; an escaping local is excluded from the release (its +1 leaks — the
// pre-existing, safe direction, unchanged by this fix).
//
// OBSERVABILITY. runAotChecked links + runs the generated code under ASan/LSan;
// a leaked string flips r.ran to false (and a double-free / UAF from
// over-releasing would too). The exit code additionally pins the round-tripped
// JSON content, so a premature free would corrupt the value as well as flip
// r.ran. Each case FAILS without the fix (LSan reports the leak demonstrated
// above) and PASSES with it (mechanically verified: reverting the
// LlvmBackend_Setup.cpp hunk and rebuilding reproduces the exact "49 byte(s)
// leaked in 2 allocation(s)" trace against this file's own (A) repro; restoring
// the fix clears it).
//
// APIs verified against Bezel_Standard_Library.md: ISerializable/
// IDeserializable (§2.14), JsonSerializer (§2.15: `constructor()`,
// `ToBytes(): owned OwningBuffer<byte>`, `ToString(): string`), string.Contains
// (§9.6).

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

const char* kImport = "import System.Serialization; ";

// A minimal ISerializable/IDeserializable class (§2.14) — synthesized
// Serialize/Deserialize per §9.8, no hand-written body needed.
const char* kPoint =
    "class Point : ISerializable, IDeserializable "
    "  public property Name: string; "
    "  public constructor(name: string) Name := name; end constructor; "
    "end Point; ";

}  // namespace

// =====================================================================
// (A) The exact #2435 repro shape: `var json := s.ToString()` on a
// JsonSerializer that has just written one object, consumed non-escaping
// (a `.Contains` read). Pre-fix: LSan reports "49 byte(s) leaked in 2
// allocation(s)" rooted at bz_jser_to_string <- main. stdlib §2.15 / §28.9.
// =====================================================================
BEZEL_TEST(json_serializer_tostring_leak_2435,
           bind_tostring_lsan_clean_stdlib_2_15) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kImport + kPoint +
        "class C public static method Main(): int "
        "  using var p := new Point(\"hello\"); "
        "  using var s := new JsonSerializer(); "
        "  p.Serialize(s); "
        "  var json: string := s.ToString(); "
        "  if json.Contains(\"hello\") then return 42; end; "
        "  return 0; "
        "end Main; end C;");
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);        // compiled + LSan-clean (result freed once)
    BEZEL_EXPECT_EQ(r.exitCode, 42); // round-tripped JSON content, value intact
}

// =====================================================================
// (B) ESCAPING-local control — the fix's own double-free risk axis. `json`
// in MakeJson is RETURNED (an escape per the StringEscapeWalker), so it must
// be EXCLUDED from MakeJson's own scope-end release (a release there would
// double-free once the caller also holds it). The ReturnStmt lowering's
// `returnedStringLocalOwnsPlusOne` must instead recognize `json`'s +1-producer
// initializer and TRANSFER (not mint a second) reference to the caller, which
// binds it via the pre-existing `isUserMethodStringReturnCall` producer path
// and releases it once at Main's scope end. Pre-fix this was already
// borrow-then-release-at-MakeJson's-end (no double-free, but also a leak, per
// (A)'s trace) — this control pins that TRANSITIONING to the transfer
// discipline across a function boundary introduces no double-free while still
// fixing the leak. stdlib §2.15 / language spec §4.6 / §28.9.
// =====================================================================
BEZEL_TEST(json_serializer_tostring_leak_2435,
           tostring_return_escape_no_double_free_stdlib_2_15) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kImport + kPoint +
        "class C "
        "  public static method MakeJson(): string "
        "    using var p := new Point(\"hello\"); "
        "    using var s := new JsonSerializer(); "
        "    p.Serialize(s); "
        "    var json: string := s.ToString(); "
        "    return json; "
        "  end MakeJson; "
        "  public static method Main(): int "
        "    var j := C.MakeJson(); "
        "    if j.Contains(\"hello\") then return 42; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);        // neither leaked nor double-freed
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// =====================================================================
// (C) Immediate inline consumption — NO local var at all: `s.ToString()` is
// the direct receiver of `.Length` (CG27a's string-member-access path,
// LlvmBackend_Exprs.cpp). This is a SEPARATE consumer of isPlusOneStringProducer
// from the LocalDecl scope-end path (A) exercises: it releases the temporary
// immediately after the length read. Confirms the fix's recognition also
// closes the no-local-var leak shape, not only the bound-local one.
// stdlib §2.15 / §4.6 / §28.9.
// =====================================================================
BEZEL_TEST(json_serializer_tostring_leak_2435,
           tostring_inline_length_lsan_clean_stdlib_2_15) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kImport + kPoint +
        "class C public static method Main(): int "
        "  using var p := new Point(\"hello\"); "
        "  using var s := new JsonSerializer(); "
        "  p.Serialize(s); "
        "  return s.ToString().Length; "
        "end Main; end C;");
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);
    // '{"Name":"hello"}' — pin the exact synthesized JSON length so a
    // premature free (which would corrupt or truncate the buffer) is caught
    // by the value, not merely by r.ran.
    BEZEL_EXPECT_EQ(r.exitCode, static_cast<int>(
        std::string("{\"Name\":\"hello\"}").size()));
}

// =====================================================================
// (D) Two independent ToString() calls in the same scope — each call mints
// an INDEPENDENT fresh rc=1 string (bz_jser_to_string does not clear the
// serializer's buffer, unlike bz_binser_to_bytes/bz_jser_to_bytes), so both
// must be released independently at scope end with no interference between
// the two slots. stdlib §2.15 / §4.6 / §28.9.
// =====================================================================
BEZEL_TEST(json_serializer_tostring_leak_2435,
           tostring_called_twice_both_released_stdlib_2_15) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kImport + kPoint +
        "class C public static method Main(): int "
        "  using var p := new Point(\"hello\"); "
        "  using var s := new JsonSerializer(); "
        "  p.Serialize(s); "
        "  var a: string := s.ToString(); "
        "  var b: string := s.ToString(); "
        "  if a.Contains(\"hello\") and b.Contains(\"hello\") then return 42; end; "
        "  return 0; "
        "end Main; end C;");
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// =====================================================================
// (E) Sibling-path control — `ToBytes()` into a `using var`-disposed
// `owned OwningBuffer<byte>` was already LSan-clean (the issue's own
// discriminator between the two paths) and must STAY clean: this fix touches
// only the `ToString()` recognizer, not `ToBytes()`. stdlib §2.15.
// =====================================================================
BEZEL_TEST(json_serializer_tostring_leak_2435,
           tobytes_sibling_path_stays_lsan_clean_stdlib_2_15) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kImport + kPoint +
        "class C public static method Main(): int "
        "  using var p := new Point(\"hello\"); "
        "  using var s := new JsonSerializer(); "
        "  p.Serialize(s); "
        "  using var buf := s.ToBytes(); "
        "  return buf.Length; "
        "end Main; end C;");
    // #3733: the toolchain-SKIP guard is GONE — a SKIP (ran=false) now
    // FAILS the r.ran assertion below instead of passing silently.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, static_cast<int>(
        std::string("{\"Name\":\"hello\"}").size()));
}
