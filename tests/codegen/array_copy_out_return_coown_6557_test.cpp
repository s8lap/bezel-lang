// #6557 follow-up — `return arr[i];` out of a co-owning array that dies at the
// return handed the caller a DANGLING Key, and since #6557 a caller that
// merely stores that entry crashes (`free(): invalid pointer`).
//
// THE RULE (spec §28.11 *The element slot policy*, indexer-get row, v0.357):
// "copy out (a runtime-managed reference the copy holds — a
// `DictionaryEntry<string, V>`'s `Key` — keeps §28.9's unbounded lifetime
// through every later mutation or disposal of the collection)". `string` is
// runtime-managed (§28.9, v0.357), so the entry a method returns out of its
// own array must keep a live Key after that array is disposed.
//
// THE DEFECT. A by-value return is a bit copy, so
//     var arr := MakeDict(n).ToArray();  return arr[0];
// returned the bytes of a slot whose Key the co-owning snapshot `arr` gave
// back when it was disposed at that same return. Reading the Key was a
// use-after-free on every revision; since #6557 (the co-owner stamp survives
// `Clear`, and a user-built `Array<DictionaryEntry<string, V>>` is stamped at
// `new`) STORING it is a crash too: the array's `Add` retains freed memory
// and its next `Clear` frees it again. MEASURED on r30-c1 80a5fb9c7: every row
// below aborts with `free(): invalid pointer` (exit 134) or reads a garbage
// Key length (`System.OverflowException`, exit 70); under ASan,
// heap-use-after-free / SEGV in `bz_string_release` <- `bz_array_clear`.
// It is also what turned the sema row
// `primitive_to_value_builtin_record_hole_6565.issue_repro_with_a_real_entry_still_runs_spec_4_4`
// red.
//
// THE FIX. The return of an indexer-get whose receiver dies at the return (a
// local of the method, or a temporary) takes one reference per embedded string
// on behalf of the returned copy, before the return's disposals free the array
// (`retainCoOwnedArrayCopyOutForReturn`, LlvmBackend_Assign.cpp).
//
// ORACLE. `runAot` — the real AOT path; under `BEZEL_AOT_FORCE_SANITIZE=address`
// the use-after-free is a sanitizer finding. NOT `runAotChecked`: no caller
// releases a by-value result's strings (the same safe-direction leak as an
// unbound `d.First()` entry, #8267), so the fix leaves one retained Key per
// call, which strict LeakSanitizer would report. Every row loops 100 times and
// reads every stored Key, so a dangling one cannot hide.

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// `V` the entry's value type, `val` its value expression (over `n`), `key0`
// the first Key's expression, `entryBody` the body of
// `P.MakeEntry(n): owned DictionaryEntry<string, V>`, `loopBody` the body of
// the 100-iteration loop in `Main`, which accumulates into `t`.
std::string program(const std::string& V, const std::string& val,
                    const std::string& key0, const std::string& entryBody,
                    const std::string& loopBody) {
    return
        "module M; "
        "record R(N: string, X: int); "
        "class P "
        "  public static method MakeDict(n: int): owned Dictionary<string, " + V + "> "
        "    var d := new Dictionary<string, " + V + ">(); "
        "    d.Add(" + key0 + ", " + val + "); d.Add($\"bb{n}\", " + val + "); "
        "    return d; "
        "  end MakeDict; "
        "  public static method MakeEntry(n: int): owned DictionaryEntry<string, " + V + "> "
        "    " + entryBody + " "
        "  end MakeEntry; "
        "end P; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; var t := 0; "
        "    while i < 100 do "
        "      " + loopBody + " "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end Main; "
        "end C; ";
}

const char* kLocalSnapshotReturn =
    "var arr := MakeDict(n).ToArray(); return arr[0];";

void expectRuns(const std::string& prog, const char* expected) {
    JitResult r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(expected));
}

}  // namespace

// A user-built array (stamped at `new` since #6557) stores the returned entry
// and reads its Key back. Keys `a{i}`: 10*2 + 90*3 = 290.
BEZEL_TEST(array_copy_out_return_coown_6557,
           user_built_array_stores_a_returned_entry_spec_28_11) {
    expectRuns(program("int", "1", "$\"a{n}\"", kLocalSnapshotReturn,
                       "var es := new Array<DictionaryEntry<string, int>>(); "
                       "es.Add(P.MakeEntry(i)); t := t + es[0].Key.Length; "
                       "es.Clear();"),
               "290\n");
}

// The sema row's own shape — a `ToArray()` snapshot cleared, refilled with the
// returned entry, cleared twice — with a `string` value and a Key read.
BEZEL_TEST(array_copy_out_return_coown_6557,
           snapshot_clear_then_add_returned_entry_string_value_spec_28_11) {
    expectRuns(program("string", "$\"v{n}\"", "$\"a{n}\"",
                       kLocalSnapshotReturn,
                       "var arr := P.MakeDict(i).ToArray(); arr.Clear(); "
                       "arr.Add(P.MakeEntry(i)); t := t + arr[0].Key.Length; "
                       "arr.Clear(); arr.Clear();"),
               "290\n");
}

// A record value and a LITERAL first Key: the Key the snapshot carries is
// still a rebuilt heap string, so it dangles the same way. 100 * 2 = 200.
BEZEL_TEST(array_copy_out_return_coown_6557,
           record_value_literal_key_returned_entry_spec_28_11) {
    expectRuns(program("R", "new R($\"r{n}\", 1)", "\"aa\"",
                       kLocalSnapshotReturn,
                       "var es := new Array<DictionaryEntry<string, R>>(); "
                       "es.Add(P.MakeEntry(i)); t := t + es[0].Key.Length; "
                       "es.Clear();"),
               "200\n");
}

// The receiver is a TEMPORARY (`MakeDict(n).ToArray()[0]`), freed at the end
// of the return statement.
BEZEL_TEST(array_copy_out_return_coown_6557,
           temporary_receiver_returned_entry_spec_28_11) {
    expectRuns(program("int", "1", "$\"a{n}\"",
                       "return MakeDict(n).ToArray()[0];",
                       "var arr := P.MakeDict(i).ToArray(); arr.Clear(); "
                       "arr.Add(P.MakeEntry(i)); t := t + arr[0].Key.Length; "
                       "arr.Clear();"),
               "290\n");
}

// The receiver is a USER-BUILT local filled by a for-in, and the caller only
// READS the returned Key — the pre-#6557 half of the same defect.
BEZEL_TEST(array_copy_out_return_coown_6557,
           read_key_of_entry_returned_from_user_built_local_spec_28_9) {
    expectRuns(program("int", "1", "$\"a{n}\"",
                       "var es := new Array<DictionaryEntry<string, int>>(); "
                       "for var e in MakeDict(n) do es.Add(e); end for; "
                       "return es[0];",
                       "t := t + P.MakeEntry(i).Key.Length;"),
               "290\n");
}
