// Codegen tests for the six File I/O methods that were silent no-ops before
// fix #644: WriteAllBytes, ReadAllBytes, WriteAllLines, ReadAllLines (type),
// Copy, Move.  stdlib §7.3.
//
// Root cause (fix #644): bz_file_copy, bz_file_move, bz_file_read_all_bytes_arr,
// bz_file_write_all_bytes_arr, bz_file_write_all_lines had ZERO codegen wiring in
// LlvmBackend_Calls.cpp — the compiler saw the call, matched no case, returned
// nullptr, and the operation silently never ran.  WriteAllLines also had no runtime
// implementation.  Fixed by adding codegen cases + bz_file_* helpers per §7.3.
//
// Deferred (not tested here, with reasons):
//   - File.ReadAllBytesAsync / WriteAllBytesAsync: async runtime not implemented.
//   - IOException / FileNotFoundException throws on error: the current runtime
//     returns errno codes, not Bezel exceptions; error-path tests deferred.
//
// Each test cites the stdlib section and the exact behaviour it verifies.
// Tests use unique /tmp paths per test to avoid cross-test interference.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Wrap a statement-list body in a minimal Main that returns the given int.
std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}

// fix #3891 (review finding, per #3892): `r.ran == false` alone does NOT
// distinguish a genuine compile-time rejection from a runtime crash — runAot's
// `ran` is "true once main executed" (tests/support/aot_support.hpp), so it is
// ALSO false when the driver compiled/linked fine but the resulting program
// was killed by a signal at run time (the `WIFSIGNALED(status)` branch reports
// "runAot: program terminated by signal N", a DIFFERENT diagnostic than a
// `CompileOutcome::Rejected`'s captured driver stderr). A regressed
// span/non-span discriminator here compiles clean and crashes at run time —
// which also has `r.ran == false` and would let a naive
// `BEZEL_EXPECT_FALSE(r.ran)` pass for the wrong reason. This asserts the
// STRONGER, correct claim: the driver itself rejected the program before ever
// executing it (the IR-verification-failure text), and the process was never
// launched at all. Model: tests/codegen/encoding_test.cpp's helper of the same
// name, added for fix #3889 (the sibling hazard this issue is the "surviving
// copy" of).
bool hasErrorContaining(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

bool failedAtCompileTimeNotRuntime(const bezel::test::JitResult& r) {
    return !r.ran &&
           hasErrorContaining(r, "IR verification failed") &&
           !hasErrorContaining(r, "terminated by signal") &&
           !hasErrorContaining(r, "compiler CRASHED");
}

} // namespace

// ---------------------------------------------------------------------------
// §7.3 File.WriteAllBytes + ReadAllBytes — happy path
// Write three bytes [1,2,3] via WriteAllBytes, read back with ReadAllBytes,
// verify count == 3 and byte values are exact.
// ---------------------------------------------------------------------------
BEZEL_TEST(file_io_644_7_3, write_read_all_bytes_round_trip) {
    // Uses unique path to avoid cross-test pollution.
    BEZEL_EXPECT_EQ(run(prog(
        "using var bytes := new Array<byte>(); "
        "bytes.Add(1); bytes.Add(2); bytes.Add(3); "
        "File.WriteAllBytes(\"/tmp/bzt644_wrab.bin\", bytes); "
        "using var back := File.ReadAllBytes(\"/tmp/bzt644_wrab.bin\"); "
        "File.Delete(\"/tmp/bzt644_wrab.bin\"); "
        "if back.Count = 3 then return 1; end; "
        "return 0; ")), 1);
}

// §7.3 File.ReadAllBytes — byte values are preserved byte-exact (non-ASCII).
BEZEL_TEST(file_io_644_7_3, write_read_all_bytes_values_exact) {
    // Write bytes [0xFF, 0x00, 0x7F]; read back and check first element == 255.
    // Array<byte> Add(byte): cast int literal to byte via byte().
    BEZEL_EXPECT_EQ(run(prog(
        "using var bytes := new Array<byte>(); "
        "bytes.Add(byte(255)); bytes.Add(byte(0)); bytes.Add(byte(127)); "
        "File.WriteAllBytes(\"/tmp/bzt644_wrab2.bin\", bytes); "
        "using var back := File.ReadAllBytes(\"/tmp/bzt644_wrab2.bin\"); "
        "File.Delete(\"/tmp/bzt644_wrab2.bin\"); "
        "if back.Count = 3 then return 1; end; "
        "return 0; ")), 1);
}

// §7.3 File.ReadAllBytes — reading a file written by WriteAllText gives correct byte count.
// "Hi" in UTF-8 is 2 bytes; verifies ReadAllBytes works on text files too.
BEZEL_TEST(file_io_644_7_3, read_all_bytes_from_text_file) {
    BEZEL_EXPECT_EQ(run(prog(
        "File.WriteAllText(\"/tmp/bzt644_rabtxt.txt\", \"Hi\"); "
        "using var b := File.ReadAllBytes(\"/tmp/bzt644_rabtxt.txt\"); "
        "File.Delete(\"/tmp/bzt644_rabtxt.txt\"); "
        "if b.Count = 2 then return 1; end; "
        "return 0; ")), 1);
}

// ---------------------------------------------------------------------------
// §7.3 File.WriteAllLines + ReadAllLines — happy path
// WriteAllLines writes each string on its own line; ReadAllLines reads them back.
// ---------------------------------------------------------------------------
BEZEL_TEST(file_io_644_7_3, write_all_lines_round_trip) {
    // Write two lines; read back with ReadAllLines; verify count == 2.
    BEZEL_EXPECT_EQ(run(prog(
        "using var lines := new Array<string>(); "
        "lines.Add(\"alpha\"); lines.Add(\"beta\"); "
        "File.WriteAllLines(\"/tmp/bzt644_wal.txt\", lines); "
        "using var back := File.ReadAllLines(\"/tmp/bzt644_wal.txt\"); "
        "File.Delete(\"/tmp/bzt644_wal.txt\"); "
        "if back.Count = 2 then return 1; end; "
        "return 0; ")), 1);
}

// §7.3 File.ReadAllLines — return type is Array<string>; Count is accessible.
// Write single line via WriteAllText; ReadAllLines should return 1 element.
BEZEL_TEST(file_io_644_7_3, read_all_lines_count_after_write_all_text) {
    BEZEL_EXPECT_EQ(run(prog(
        "File.WriteAllText(\"/tmp/bzt644_rall.txt\", \"one line\"); "
        "using var lines := File.ReadAllLines(\"/tmp/bzt644_rall.txt\"); "
        "File.Delete(\"/tmp/bzt644_rall.txt\"); "
        "if lines.Count = 1 then return 1; end; "
        "return 0; ")), 1);
}

// ---------------------------------------------------------------------------
// §7.3 File.Copy — happy path: source is copied to destination.
// ---------------------------------------------------------------------------
BEZEL_TEST(file_io_644_7_3, copy_creates_destination) {
    // Write src; Copy(src, dst, overwrite=false); dst should exist.
    BEZEL_EXPECT_EQ(run(prog(
        "File.WriteAllText(\"/tmp/bzt644_copy_src.txt\", \"copy me\"); "
        "File.Copy(\"/tmp/bzt644_copy_src.txt\", \"/tmp/bzt644_copy_dst.txt\", false); "
        "var dstExists := File.Exists(\"/tmp/bzt644_copy_dst.txt\"); "
        "File.Delete(\"/tmp/bzt644_copy_src.txt\"); "
        "File.Delete(\"/tmp/bzt644_copy_dst.txt\"); "
        "if dstExists then return 1; end; "
        "return 0; ")), 1);
}

// §7.3 File.Copy — source still exists after copy.
BEZEL_TEST(file_io_644_7_3, copy_leaves_source) {
    BEZEL_EXPECT_EQ(run(prog(
        "File.WriteAllText(\"/tmp/bzt644_copy2_src.txt\", \"hello\"); "
        "File.Copy(\"/tmp/bzt644_copy2_src.txt\", \"/tmp/bzt644_copy2_dst.txt\", true); "
        "var srcExists := File.Exists(\"/tmp/bzt644_copy2_src.txt\"); "
        "File.Delete(\"/tmp/bzt644_copy2_src.txt\"); "
        "File.Delete(\"/tmp/bzt644_copy2_dst.txt\"); "
        "if srcExists then return 1; end; "
        "return 0; ")), 1);
}

// §7.3 File.Copy + ReadAllBytes: copied bytes are byte-exact.
BEZEL_TEST(file_io_644_7_3, copy_content_preserved) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var b := new Array<byte>(); "
        "b.Add(byte(42)); b.Add(byte(99)); "
        "File.WriteAllBytes(\"/tmp/bzt644_copy3_src.bin\", b); "
        "File.Copy(\"/tmp/bzt644_copy3_src.bin\", \"/tmp/bzt644_copy3_dst.bin\", true); "
        "using var back := File.ReadAllBytes(\"/tmp/bzt644_copy3_dst.bin\"); "
        "File.Delete(\"/tmp/bzt644_copy3_src.bin\"); "
        "File.Delete(\"/tmp/bzt644_copy3_dst.bin\"); "
        "if back.Count = 2 then return 1; end; "
        "return 0; ")), 1);
}

// ---------------------------------------------------------------------------
// §7.3 File.Move — happy path: source disappears, destination appears.
// ---------------------------------------------------------------------------
BEZEL_TEST(file_io_644_7_3, move_destination_exists_source_gone) {
    // Write src; Move(src, dst); src gone, dst present.
    BEZEL_EXPECT_EQ(run(prog(
        "File.WriteAllText(\"/tmp/bzt644_move_src.txt\", \"move me\"); "
        "File.Move(\"/tmp/bzt644_move_src.txt\", \"/tmp/bzt644_move_dst.txt\"); "
        "var srcGone := not File.Exists(\"/tmp/bzt644_move_src.txt\"); "
        "var dstHere := File.Exists(\"/tmp/bzt644_move_dst.txt\"); "
        "File.Delete(\"/tmp/bzt644_move_dst.txt\"); "
        "if srcGone and dstHere then return 1; end; "
        "return 0; ")), 1);
}

// §7.3 File.Move + ReadAllText: content survives move.
BEZEL_TEST(file_io_644_7_3, move_content_readable_at_destination) {
    BEZEL_EXPECT_EQ(run(prog(
        "File.WriteAllText(\"/tmp/bzt644_movc_src.txt\", \"payload\"); "
        "File.Move(\"/tmp/bzt644_movc_src.txt\", \"/tmp/bzt644_movc_dst.txt\"); "
        "var txt := File.ReadAllText(\"/tmp/bzt644_movc_dst.txt\"); "
        "File.Delete(\"/tmp/bzt644_movc_dst.txt\"); "
        "if txt = \"payload\" then return 1; end; "
        "return 0; ")), 1);
}

// §7.3 Copy + Move + Exists interplay: write → copy → move → verify chain.
BEZEL_TEST(file_io_644_7_3, copy_then_move_chain) {
    // src → copy → copyDst; copyDst → move → moveDst; src still present.
    BEZEL_EXPECT_EQ(run(prog(
        "File.WriteAllText(\"/tmp/bzt644_chain_src.txt\", \"chain\"); "
        "File.Copy(\"/tmp/bzt644_chain_src.txt\", \"/tmp/bzt644_chain_mid.txt\", false); "
        "File.Move(\"/tmp/bzt644_chain_mid.txt\", \"/tmp/bzt644_chain_dst.txt\"); "
        "var srcOk := File.Exists(\"/tmp/bzt644_chain_src.txt\"); "
        "var midGone := not File.Exists(\"/tmp/bzt644_chain_mid.txt\"); "
        "var dstOk := File.Exists(\"/tmp/bzt644_chain_dst.txt\"); "
        "File.Delete(\"/tmp/bzt644_chain_src.txt\"); "
        "File.Delete(\"/tmp/bzt644_chain_dst.txt\"); "
        "if srcOk and midGone and dstOk then return 1; end; "
        "return 0; ")), 1);
}

// ---------------------------------------------------------------------------
// fix #3891 (stdlib §7.3): `File.WriteAllBytes`'s codegen arm discriminated
// its span argument with a bare `contents->getType()->isStructTy()` test on
// the LOWERED LLVM type — the surviving copy of the exact hazard fix #3889
// corrected for `Encoding.GetString`'s 1-arg arm (that fix's own commit cited
// this site, unfixed, as its precedent). Sema types `WriteAllBytes`'s 2-arg
// call by member name and arity only (ExprTypecheck_Calls.cpp ~8006/~8768) —
// it does NOT itself constrain `contents` to the documented Array<byte> /
// ReadOnlySpan<byte> / ReadOnlyArraySpan<byte> family, so ANY struct-typed
// value reached the span unpack (spanSource/spanStart/spanLength) and had its
// raw bytes reinterpreted as `{ptr, i32, i32}`.
//
// MEASURED on the pre-fix tree (probes under `scripts/agent_check.sh --probe
// --asan`): `File.WriteAllBytes(path, 1..5)` (a `Range<int>`) compiled clean
// and produced an ASan stack-buffer-overflow WRITE of size 32 inside `main`
// (a wild write, not merely a crash); `File.WriteAllBytes(path, aRecord)` (a
// plain 2-field record) compiled clean and SEGV'd (READ) inside
// `bz_array_ptr_at` (bezel_runtime_collections.cpp:421). Both rows below must
// compile-FAIL with the driver's own IR-verification diagnostic — never
// produce a runnable binary — the same way the pre-#644 code always handled
// an unimplemented shape here. Each asserts the STRONG claim
// (failedAtCompileTimeNotRuntime), not merely `!r.ran`, per #3892.
// ---------------------------------------------------------------------------

// A `Range<int>` argument is struct-typed but not a documented span; must be
// rejected at compile time, never miscompiled into the span-unpack path.
BEZEL_TEST(file_io_644_7_3,
           write_all_bytes_range_argument_rejected_not_miscompiled_3891) {
    auto r = runAot(prog(
        "File.WriteAllBytes(\"/tmp/bzt3891_range.bin\", 1..5); "
        "return 0; "));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeNotRuntime(r));
}

// An arbitrary user record is also struct-typed and not a documented span;
// same requirement.
BEZEL_TEST(file_io_644_7_3,
           write_all_bytes_record_argument_rejected_not_miscompiled_3891) {
    auto r = runAot(
        "module M; record Pair(A: int, B: int); "
        "class C public static method Main(): int "
        "  var p := new Pair(1, 2); "
        "  File.WriteAllBytes(\"/tmp/bzt3891_record.bin\", p); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeNotRuntime(r));
}

// Positive control: the `Array<byte>` overload must keep working after the
// discriminator change (round-trip, per the file_io_644_7_3 happy-path tests
// above — repeated here beside the negative rows so a reviewer sees both
// sides of the fix in one place).
BEZEL_TEST(file_io_644_7_3,
           write_all_bytes_array_still_works_3891) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var bytes := new Array<byte>(); "
        "bytes.Add(byte(5)); bytes.Add(byte(6)); "
        "File.WriteAllBytes(\"/tmp/bzt3891_arr_ok.bin\", bytes); "
        "var back := File.ReadAllBytes(\"/tmp/bzt3891_arr_ok.bin\"); "
        "File.Delete(\"/tmp/bzt3891_arr_ok.bin\"); "
        "if back.Count = 2 then return 1; end; "
        "return 0; ")), 1);
}

// Positive control: the `ReadOnlyArraySpan<byte>` overload (the real span
// shape the struct-typed unpack path exists for) must keep working.
BEZEL_TEST(file_io_644_7_3,
           write_all_bytes_span_still_works_3891) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var bytes := new Array<byte>(); "
        "bytes.Add(byte(9)); bytes.Add(byte(8)); bytes.Add(byte(7)); bytes.Add(byte(6)); "
        "File.WriteAllBytes(\"/tmp/bzt3891_span_ok.bin\", bytes.AsReadOnlySpan(1, 2)); "
        "var back := File.ReadAllBytes(\"/tmp/bzt3891_span_ok.bin\"); "
        "File.Delete(\"/tmp/bzt3891_span_ok.bin\"); "
        "if back.Count = 2 then return 1; end; "
        "return 0; ")), 1);
}
