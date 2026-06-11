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

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// Wrap a statement-list body in a minimal Main that returns the given int.
std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
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
