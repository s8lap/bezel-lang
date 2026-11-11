// #6566 — a record or `DictionaryEntry<string, V>` LOCAL copied out of an array
// with the indexer-get held no reference to the strings it embeds, so any
// later array operation that gave such a string back left the local dangling —
// and storing the local back into the array handed the array a dead string.
//
// THE RULE (spec §28.11 "The element slot policy", operations table, copy-slot
// column, row "indexer-get / default iteration"; §28.9 "Runtime-managed types";
// v0.357). The indexer-get of a value element is a
//
//     "copy out (a runtime-managed reference the copy holds — a
//      `DictionaryEntry<string, V>`'s `Key` — keeps §28.9's unbounded lifetime
//      through every later mutation or disposal of the collection)"
//
// so a string reached through the copy reads back as it was copied, whatever
// the program then does to the array.
//
// THE DEFECT. The implementation realizes "runtime-managed" by refcount. A
// co-owning array (#6550 a `ToArray()` snapshot, #6557 a user-built
// `Array<DictionaryEntry<string, V>>`, #8386 an `Array<E>` of records) holds
// one reference per embedded string per slot, and gives it back when the slot
// is overwritten, removed or cleared. The copy `var t := rows[i]` was sixteen
// plain bytes: it took no reference. So in
//
//     var t := rows[0]; rows[0] := rows[1]; rows[1] := t;      // a swap
//     var t := rows[0]; rows.RemoveAt(0); rows.Add(t);         // a rotate
//     var b := rows[2]; rows.Clear(); rows.Add(b);             // keep one
//
// the array gave the string's last reference back while `t` still named it,
// and the store of `t` then retained freed memory.
//
// MEASURED on the lane's own previous tip (the #8386 commit, which
// is what made an `Array<E>` a co-owner): each of the three forms above, with
// the Names read out of a `Split` result that has since died and NEVER read
// again, aborted — `bezel: fatal: refcount over-release (string)` or
// `free(): invalid pointer`, rc=134 — where the base ran clean and printed
// the right total (origin/main `e49c32aca` here; the #8386 review measured
// r30-c1 @ `32207125`). The reading forms (`t.Name` after the store)
// were a heap-use-after-free on the base as well.
//
// THE FIX. A local bound (or re-bound) from an array indexer-get of a
// string-embedding value element gets a shadow copy that takes one reference
// per embedded string when the array is a co-owner
// (`bz_array_coown_copy_out`), and gives them back when the local's scope
// ends, on every exit. An array that is NOT a co-owner yields no reference:
// its slots may name strings that are already dead, and retaining through
// them would itself be the fault.
//
// ORACLE. Every row runs under `runAotChecked` — ASan plus strict LeakSanitizer
// — and asserts the exact stdout, that the program RAN TO COMPLETION, exit 0,
// and that nothing leaked or was over-released; the F1 rows run a second,
// plain `runAot` leg asserting rc=0 and the same stdout (the abort was
// observable without a sanitizer). Every row loops 200 times. The expected
// totals are computed from the program text, not read off the compiler.
//
// FAIL-WITHOUT (measured against the direct parent, the #8386 commit): 31 of
// the 35 rows are red there. The four that are green on both arms are guards
// against the fix's own failure modes, not repros: the two "not a co-owner"
// rows (a retain through a dead string), `local_rebound_fifty_times_...` and
// `copies_bound_and_dropped_...` (an unpaired take or release).
//
// NOT COVERED HERE (each measured; see the commit body): a copy that reaches
// its local by another road — the default-iteration variable copied to an
// outer local, a method's return value, a class field, a second local — and a
// copy bound inside an `async` or iterator body are not given a reference by
// this commit.

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The harness SKIPS (loudly, counted) when clang is absent; a sanitizer finding
// is NOT a skip — "sanitizer fired" means it ran and found something.
bool skipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("SKIP") == std::string::npos) continue;
        if (d.message.find("sanitizer fired") != std::string::npos) return false;
        return true;
    }
    return false;
}

// Crash oracle AND leak oracle in one row: the program must run to completion
// under ASan, print exactly `expected`, exit 0, and report neither a leak nor
// an over-release.
void expectCleanRun(const std::string& program, const char* expected) {
    JitResult r = runAotChecked(program);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(expected));
    for (const auto& d : r.diagnostics.diagnostics()) {
        BEZEL_EXPECT_TRUE(d.message.find("over-release") == std::string::npos);
        BEZEL_EXPECT_TRUE(d.message.find("byte(s) leaked") == std::string::npos);
        BEZEL_EXPECT_TRUE(d.message.find("use-after-free") == std::string::npos);
    }
}

// The plain leg: the shipping AOT path with no sanitizer asked for. The
// regression was an abort (rc=134) there too, so rc=0 and the exact stdout
// are a real oracle on their own.
void expectPlainRun(const std::string& program, const char* expected) {
    JitResult r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(expected));
}

void expectCleanBothLegs(const std::string& program, const char* expected) {
    expectCleanRun(program, expected);
    expectPlainRun(program, expected);
}

// The review's F1 programs. `Build(i)` returns a three-element `Array<E>`
// whose Names (`name{i}`, `zz{i}`, `q{i}`; N = 1, 2, 3) are read out of a
// `Split` result that dies when `Build` returns, so from then on the array is
// the only thing keeping them alive. `ops` mutates `rows` through a local
// copied out of it; `Junk()` then reuses whatever was freed; `tail` reads the
// array back.
const char* const kReadN =
    "      for var r in rows do total := total + r.N; end;\n";
const char* const kReadNames =
    "      for var r in rows do total := total + r.Name.Length + r.N; end;\n"
    "      total := total + rows.Count;\n";

std::string recordProgram(const std::string& ops, const char* tail,
                          const char* build = "Build(i)") {
    return std::string(
        "module P;\n"
        "record E(Name: string, N: int);\n"
        "record W(Inner: E, Tag: string);\n"
        "public class Program\n"
        "  static method Build(i: int): owned Array<E>\n"
        "    var rows := new Array<E>();\n"
        "    var f := $\"name{i},zz{i},q{i}\".Split(',');\n"
        "    rows.Add(new E(f[0], 1)); rows.Add(new E(f[1], 2)); rows.Add(new E(f[2], 3));\n"
        "    return rows;\n"
        "  end Build;\n"
        "  static method BuildW(i: int): owned Array<W>\n"
        "    var rows := new Array<W>();\n"
        "    var f := $\"wn{i},wz{i},wq{i},ta{i},tbb{i},tccc{i}\".Split(',');\n"
        "    rows.Add(new W(new E(f[0], 1), f[3])); rows.Add(new W(new E(f[1], 2), f[4]));\n"
        "    rows.Add(new W(new E(f[2], 3), f[5]));\n"
        "    return rows;\n"
        "  end BuildW;\n"
        "  static method Lit(i: int): owned Array<E>\n"
        "    var f := $\"lit{i},lz{i}\".Split(',');\n"
        "    var rows: Array<E> := [new E(f[0], 1), new E(f[1], 2)];\n"
        "    return rows;\n"
        "  end Lit;\n"
        "  static method Swap<T>(a: Array<T>, i: int, j: int)\n"
        "    var t := a[i]; a[i] := a[j]; a[j] := t;\n"
        "  end Swap;\n"
        "  static method SwapL(a: IList<E>, i: int, j: int)\n"
        "    var t := a[i]; a[i] := a[j]; a[j] := t;\n"
        "  end SwapL;\n"
        "  static method Junk()\n"
        "    var junk := new Array<string>();\n"
        "    for var k in 1..50 do junk.Add($\"junkjunkjunk{k}\"); end;\n"
        "  end Junk;\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 1..200 do\n"
        "      var rows := ") + build + ";\n" +
        ops +
        "      Program.Junk();\n" +
        tail +
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
}

// The entry programs. `d` holds three entries (`a0`→1, `bb0`→2, `c`→3) with
// interpolated Keys; `body` builds an `Array<DictionaryEntry<string, int>>`
// from it each iteration and works on that.
std::string entryProgram(const std::string& body,
                         const char* adds = "d.Add($\"a{n}\", 1); d.Add($\"bb{n}\", 2); d.Add(\"c\", 3);") {
    return std::string(
        "module M; class C\n"
        "  static method Junk()\n"
        "    var junk := new Array<string>();\n"
        "    for var k in 1..50 do junk.Add($\"junkjunkjunk{k}\"); end;\n"
        "  end Junk;\n"
        "  public static method Main(): int\n"
        "    var n := 0; var i := 0;\n"
        "    var d := new Dictionary<string, int>();\n"
        "    ") + adds + "\n"
        "    while i < 200 do\n" +
        body +
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(n);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// The review's F1 programs, VERBATIM (`r42-rev-f2-8386`, probes
// `s_swap_read_n_only`, `s_rotate_read_n_only`, `s_keep_best_clear_n_only`):
// only the one `ops` line differs between them. The array's Names come out of
// a `Split` result that died in `Build`, and only `N` is read back.
std::string reviewProgram(const char* ops) {
    return std::string(
        "module P;\n"
        "record E(Name: string, N: int);\n"
        "record Big(S: string, A1: long, A2: long, A3: long, A4: long, A5: long, A6: long, A7: long, A8: long, A9: long, A10: long, A11: long, A12: long, A13: long, A14: long, A15: long, A16: long, A17: long, A18: long, A19: long, A20: long, A21: long, A22: long, A23: long, A24: long, A25: long, A26: long, A27: long, A28: long, A29: long, A30: long, A31: long, A32: long, A33: long, A34: long, A35: long, A36: long, T: string);\n"
        "public class Holder\n"
        "  public owned Rows: Array<E> := new Array<E>();\n"
        "  public constructor() end;\n"
        "end Holder;\n"
        "public class Program\n"
        "  static method Put(rows: Array<E>, i: int)\n"
        "    var f := $\"name{i},zz{i}\".Split(',');\n"
        "    rows.Add(new E(f[0], i));\n"
        "  end Put;\n"
        "  static method Build(i: int): owned Array<E>\n"
        "    var rows := new Array<E>();\n"
        "    var f := $\"name{i},zz{i},q{i}\".Split(',');\n"
        "    rows.Add(new E(f[0], 1)); rows.Add(new E(f[1], 2)); rows.Add(new E(f[2], 3));\n"
        "    return rows;\n"
        "  end Build;\n"
        "  static method MutIList(l: IList<E>, i: int)\n"
        "    var f := $\"il{i},jl{i}\".Split(',');\n"
        "    l.Add(new E(f[0], 4)); l[0] := new E(f[1], 5); l.Insert(1, new E(f[0], 6)); l.Remove(new E(f[0], 6));\n"
        "  end MutIList;\n"
        "  static method GenMut<T>(a: Array<T>)\n"
        "    a.Insert(0, a[1]); a.RemoveAt(2); a[1] := a[0]; a.Add(a[2]);\n"
        "  end GenMut;\n"
        "  static method GenBuild<T>(x: T, y: T): owned Array<T>\n"
        "    var a := new Array<T>(); a.Add(x); a.Insert(0, y); a.Add(x); a.RemoveAt(1);\n"
        "    return a;\n"
        "  end GenBuild;\n"
        "  static method SnapToArray(i: int): owned Array<E>\n"
        "    var rows := Build(i);\n"
        "    return rows.ToArray();\n"
        "  end SnapToArray;\n"
        "  static method SnapSorted(i: int): owned Array<E>\n"
        "    var rows := Build(i);\n"
        "    return rows.Sorted(lambda(x, y) => y.N - x.N);\n"
        "  end SnapSorted;\n"
        "  static method SnapSlice(i: int): owned Array<E>\n"
        "    var rows := Build(i);\n"
        "    return rows[0..2];\n"
        "  end SnapSlice;\n"
        "  static method Junk()\n"
        "    var junk := new Array<string>();\n"
        "    for var k in 1..50 do junk.Add($\"junkjunkjunk{k}\"); end;\n"
        "  end Junk;\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 1..200 do\n"
        "      var rows := Build(i);\n")
        + "      " + ops + "\n" +
        "      Program.Junk();\n"
        "      for var r in rows do total := total + r.N; end;\n"
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
}

const char* const kUserEntryArray =
    "      var es := new Array<DictionaryEntry<string, int>>();\n"
    "      for var e in d do es.Add(e); end for;\n";

}  // namespace

// F1 `s_swap_read_n_only`: the swap. Each iteration reads N = 1 + 2 + 3; 200 * 6.
BEZEL_TEST(array_copy_out_local_coown_6566,
           f1_swap_through_local_names_never_read_spec_28_11) {
    expectCleanBothLegs(reviewProgram("var t := rows[0]; rows[0] := rows[1]; rows[1] := t;"), "1200\n");
}

// F1 `s_rotate_read_n_only`: the rotate. 200 * 6.
BEZEL_TEST(array_copy_out_local_coown_6566,
           f1_rotate_removeat_then_add_names_never_read_spec_28_11) {
    expectCleanBothLegs(reviewProgram("var t := rows[0]; rows.RemoveAt(0); rows.Add(t);"), "1200\n");
}

// F1 `s_keep_best_clear_n_only`: keep one, clear, put it back. 200 * 3.
BEZEL_TEST(array_copy_out_local_coown_6566,
           f1_keep_one_then_clear_names_never_read_spec_28_11) {
    expectCleanBothLegs(reviewProgram("var b := rows[2]; rows.Clear(); rows.Add(b);"), "600\n");
}

// F1 `s_entry_swap_read_value_only` (= `e_swap`), verbatim: an entry swap in a
// user-built `Array<DictionaryEntry<string, int>>`, only Value read. 200 * 6.
BEZEL_TEST(array_copy_out_local_coown_6566,
           f1_entry_swap_literal_keys_value_only_read_spec_28_11) {
    expectCleanBothLegs(
        "module M; class C\n"
        "  public static method Main(): int\n"
        "    var n := 0; var i := 0;\n"
        "    var d := new Dictionary<string, int>();\n"
        "    d.Add(\"a\", 1); d.Add(\"b\", 2); d.Add(\"c\", 3);\n"
        "    while i < 200 do\n"
        "      var es := new Array<DictionaryEntry<string, int>>();\n"
        "      for var e in d do es.Add(e); end for;\n"
        "      var t := es[0]; es[0] := es[1]; es[1] := t;\n"
        "      for var e in es do n := n + e.Value; end;\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(n);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "1200\n");
}

// F1 `e_swap_interp`, verbatim: the same with interpolated (heap) Keys.
BEZEL_TEST(array_copy_out_local_coown_6566,
           f1_entry_swap_interpolated_keys_value_only_read_spec_28_11) {
    expectCleanBothLegs(
        "module M; class C\n"
        "  public static method Main(): int\n"
        "    var n := 0; var i := 0;\n"
        "    var d := new Dictionary<string, int>();\n"
        "    d.Add($\"a{n}\", 1); d.Add($\"b{n}\", 2); d.Add(\"c\", 3);\n"
        "    while i < 200 do\n"
        "      var es := new Array<DictionaryEntry<string, int>>();\n"
        "      for var e in d do es.Add(e); end for;\n"
        "      var t := es[0]; es[0] := es[1]; es[1] := t;\n"
        "      for var e in es do n := n + e.Value; end;\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(n);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "1200\n");
}

// THE READING FORMS: the Names are read back after the mutation, so a dead
// string is a use-after-free, not merely a bad refcount.
// The swap. Per iteration: Names name{i}+zz{i}+q{i}, N = 6, Count = 3.
BEZEL_TEST(array_copy_out_local_coown_6566,
           swap_then_read_every_name_spec_28_11) {
    expectCleanBothLegs(recordProgram(
        "      var t := rows[0]; rows[0] := rows[1]; rows[1] := t;\n",
        kReadNames),
        "4676\n");
}

// The rotate.
BEZEL_TEST(array_copy_out_local_coown_6566,
           rotate_then_read_every_name_spec_28_11) {
    expectCleanBothLegs(recordProgram(
        "      var t := rows[0]; rows.RemoveAt(0); rows.Add(t);\n",
        kReadNames),
        "4676\n");
}

// Keep one, clear, put it back: q{i} + 3 + Count 1.
BEZEL_TEST(array_copy_out_local_coown_6566,
           keep_one_then_clear_then_read_its_name_spec_28_11) {
    expectCleanBothLegs(recordProgram(
        "      var b := rows[2]; rows.Clear(); rows.Add(b);\n",
        kReadNames),
        "1492\n");
}

// The copy alone keeps the string: the slot it came from is overwritten and
// the copy is never stored back. #6566's own first table row, on records.
BEZEL_TEST(array_copy_out_local_coown_6566,
           local_reads_its_name_after_its_slot_was_overwritten_spec_28_11) {
    expectCleanRun(recordProgram(
        "      var t := rows[0]; rows[0] := rows[1]; Program.Junk(); total := total + t.Name.Length;\n",
        kReadNames),
        "5768\n");
}

// "through every later mutation ... of the collection": Clear.
BEZEL_TEST(array_copy_out_local_coown_6566,
           local_reads_its_name_after_the_array_was_cleared_spec_28_11) {
    expectCleanRun(recordProgram(
        "      var t := rows[1]; rows.Clear(); Program.Junk(); total := total + t.Name.Length + t.N;\n",
        kReadNames),
        "1292\n");
}

// The swap written once, generically, instantiated at T = E.
BEZEL_TEST(array_copy_out_local_coown_6566,
           swap_in_a_generic_method_at_t_record_spec_28_11) {
    expectCleanRun(recordProgram(
        "      Program.Swap<E>(rows, 0, 2);\n",
        kReadNames),
        "4676\n");
}

// The swap through an `IList<E>` view of the same array.
BEZEL_TEST(array_copy_out_local_coown_6566,
           swap_through_an_ilist_parameter_spec_28_11) {
    expectCleanRun(recordProgram(
        "      Program.SwapL(rows, 0, 2);\n",
        kReadNames),
        "4676\n");
}

// RE-BINDING: `best := rows[k]` drops the reference the local held and takes
// the new one; the last copy survives the Clear.
BEZEL_TEST(array_copy_out_local_coown_6566,
           local_rebound_from_the_indexer_keeps_the_last_copy_spec_28_11) {
    expectCleanRun(recordProgram(
        "      var best := rows[0];\n"
        "      for var k in 1..2 do if rows[k].N > best.N then best := rows[k]; end; end;\n"
        "      rows.Clear(); rows.Add(best);\n",
        kReadNames),
        "1492\n");
}

// Fifty re-bindings interleaved with stores: the leak half of the oracle
// catches a re-bind that forgot to release, the crash half one that released
// twice.
BEZEL_TEST(array_copy_out_local_coown_6566,
           local_rebound_fifty_times_neither_leaks_nor_over_releases_spec_28_11) {
    expectCleanRun(recordProgram(
        "      var t := rows[0];\n"
        "      for var k in 1..50 do t := rows[k mod 3]; rows[(k + 1) mod 3] := t; end;\n"
        "      total := total + t.Name.Length;\n",
        kReadNames),
        "5368\n");
}

// Three copies outlive a Clear and are all stored back.
BEZEL_TEST(array_copy_out_local_coown_6566,
           three_locals_then_clear_then_add_back_spec_28_11) {
    expectCleanRun(recordProgram(
        "      var a := rows[0]; var b := rows[1]; var c := rows[2]; rows.Clear(); rows.Add(c); rows.Add(a); rows.Add(b);\n",
        kReadNames),
        "4676\n");
}

// EVERY EXIT releases: the copy's scope is left by `continue` (k = 0) and by
// `break` (k = 1), and the 1000 is never added.
BEZEL_TEST(array_copy_out_local_coown_6566,
           copy_in_a_loop_body_with_break_and_continue_spec_28_11) {
    expectCleanRun(recordProgram(
        "      for var k in 0..2 do\n"
        "        var t := rows[k];\n"
        "        if t.N = 2 then rows.RemoveAt(k); rows.Add(t); break; end;\n"
        "        if t.N = 1 then continue; end;\n"
        "        total := total + 1000;\n"
        "      end;\n",
        kReadNames),
        "4676\n");
}

// The scope is left by a throw BEFORE the copy is bound (nothing to release)
// and by a throw AFTER it (the reference is given back on the unwind).
BEZEL_TEST(array_copy_out_local_coown_6566,
           copy_scope_left_by_a_throw_before_and_after_the_bind_spec_28_11) {
    expectCleanRun(recordProgram(
        "      try\n"
        "        if i mod 3 = 0 then throw new InvalidOperationException(\"early\"); end;\n"
        "        var t := rows[0]; rows[0] := rows[1];\n"
        "        if i mod 3 = 1 then throw new InvalidOperationException(t.Name); end;\n"
        "        rows[1] := t;\n"
        "      catch ex: InvalidOperationException =>\n"
        "        total := total + ex.Message.Length;\n"
        "      end;\n",
        kReadNames),
        "5372\n");
}

// The copy is captured by a lambda and read through it after the swap.
BEZEL_TEST(array_copy_out_local_coown_6566,
           copy_captured_by_a_lambda_spec_28_11) {
    expectCleanRun(recordProgram(
        "      var t := rows[0]; rows[0] := rows[1]; rows[1] := t;\n"
        "      var g := lambda() => t.N + t.Name.Length;\n"
        "      total := total + g();\n",
        kReadNames),
        "6168\n");
}

// `W(Inner: E, Tag: string)`, six distinct strings: the copy holds a reference
// to BOTH of its strings, one of them inside the nested record.
BEZEL_TEST(array_copy_out_local_coown_6566,
           record_embedding_a_record_and_a_second_string_spec_28_11) {
    expectCleanRun(recordProgram(
        "      var t := rows[0]; rows[0] := rows[1]; rows[1] := t; rows.RemoveAt(2); rows.Add(t);\n",
        "      for var r in rows do total := total + r.Inner.Name.Length + r.Tag.Length + r.Inner.N; end;\n"
        "      total := total + rows.Count;\n", "BuildW(i)"),
        "6952\n");
}

// AN ARRAY THAT IS NOT A CO-OWNER. `Build(i).ToArray()` and a collection
// literal are byte copies whose strings are already dead here (#8484); the
// copy-out must not retain through them. Clean before, clean after.
BEZEL_TEST(array_copy_out_local_coown_6566,
           array_that_is_not_a_co_owner_yields_no_reference_toarray_spec_28_11) {
    expectCleanBothLegs(recordProgram(
        "      var t := rows[0]; rows[0] := rows[1]; rows[1] := t;\n",
        kReadN, "Program.Build(i).ToArray()"),
        "1200\n");
}

// The collection-literal twin.
BEZEL_TEST(array_copy_out_local_coown_6566,
           array_that_is_not_a_co_owner_yields_no_reference_literal_spec_28_11) {
    expectCleanBothLegs(recordProgram(
        "      var t := rows[0]; rows[0] := rows[1]; rows[1] := t;\n",
        kReadN, "Program.Lit(i)"),
        "600\n");
}

// ENTRIES. The swap in a user-built `Array<DictionaryEntry<string, int>>`,
// reading every Key back: 2 + (1+2 + 2+3 + 3+1) = 14 per iteration.
BEZEL_TEST(array_copy_out_local_coown_6566,
           entry_swap_then_read_every_key_spec_28_11) {
    expectCleanRun(entryProgram(std::string(kUserEntryArray) +
        "      var t := es[0]; es[0] := es[1]; es[1] := t;\n"
        "      C.Junk();\n"
        "      n := n + t.Key.Length;\n"
        "      for var e in es do n := n + e.Value + e.Key.Length; end;\n"),
        "2800\n");
}

// THE ISSUE'S OWN TABLE, row 1: `var t := arr[0]; arr[0] := arr[1]; t.Key` on a
// `ToArray()` snapshot — then the swap is completed.
BEZEL_TEST(array_copy_out_local_coown_6566,
           issue_6566_row_1_toarray_copy_reads_key_after_slot_overwrite_spec_28_11) {
    expectCleanRun(entryProgram(
        "      var arr := d.ToArray();\n"
        "      var t := arr[0]; arr[0] := arr[1];\n"
        "      C.Junk();\n"
        "      n := n + t.Key.Length + t.Value;\n"
        "      arr[1] := t;\n"
        "      for var e in arr do n := n + e.Value + e.Key.Length; end;\n"),
        "3000\n");
}

// THE ISSUE'S OWN TABLE, row 2: `var t := arr[0]; arr.Dispose(); t.Key` —
// "or disposal of the collection".
BEZEL_TEST(array_copy_out_local_coown_6566,
           issue_6566_row_2_toarray_copy_reads_key_after_dispose_spec_28_11) {
    expectCleanRun(entryProgram(
        "      var arr := d.ToArray();\n"
        "      var t := arr[0]; arr.Dispose();\n"
        "      C.Junk();\n"
        "      n := n + t.Key.Length + t.Value;\n"),
        "600\n");
}

// Rotate, then keep one and clear: `c`→3 survives; 3 + 1.
BEZEL_TEST(array_copy_out_local_coown_6566,
           entry_rotate_then_keep_one_then_clear_spec_28_11) {
    expectCleanRun(entryProgram(std::string(kUserEntryArray) +
        "      var t := es[0]; es.RemoveAt(0); es.Add(t);\n"
        "      var b := es[1]; es.Clear(); es.Add(b);\n"
        "      C.Junk();\n"
        "      for var e in es do n := n + e.Value + e.Key.Length; end;\n"),
        "800\n");
}

// The entry local is re-bound twice, then outlives a Clear.
BEZEL_TEST(array_copy_out_local_coown_6566,
           entry_local_rebound_from_the_indexer_spec_28_11) {
    expectCleanRun(entryProgram(std::string(kUserEntryArray) +
        "      var t := es[0];\n"
        "      for var k in 1..2 do if es[k].Value > t.Value then t := es[k]; end; end;\n"
        "      es.Clear(); C.Junk(); es.Add(t);\n"
        "      for var e in es do n := n + e.Value + e.Key.Length; end;\n"),
        "800\n");
}

// A throw from a callee unwinds THROUGH `Work`'s frame while `t` holds the
// only reference to `name{i}` (the slot was overwritten): the unwind gives it
// back (leak half), and the odd iterations complete the swap.
BEZEL_TEST(array_copy_out_local_coown_6566,
           throw_unwinds_through_the_frame_holding_the_copy_spec_28_11) {
    expectCleanRun(
        "module P;\n"
        "record E(Name: string, N: int);\n"
        "public class Program\n"
        "  static method Junk()\n"
        "    var junk := new Array<string>();\n"
        "    for var k in 1..50 do junk.Add($\"junkjunkjunk{k}\"); end;\n"
        "  end Junk;\n"
        "  static method Build(i: int): owned Array<E>\n"
        "    var rows := new Array<E>();\n"
        "    var f := $\"name{i},zz{i},q{i}\".Split(',');\n"
        "    rows.Add(new E(f[0], 1)); rows.Add(new E(f[1], 2)); rows.Add(new E(f[2], 3));\n"
        "    return rows;\n"
        "  end Build;\n"
        "  static method Boom(i: int)\n"
        "    if i mod 2 = 0 then throw new InvalidOperationException(\"boom\"); end;\n"
        "  end Boom;\n"
        "  static method Work(rows: Array<E>, i: int)\n"
        "    var t := rows[0]; rows[0] := rows[1];\n"
        "    Program.Boom(i);\n"
        "    rows[1] := t;\n"
        "  end Work;\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 1..200 do\n"
        "      var rows := Build(i);\n"
        "      try\n"
        "        Program.Work(rows, i);\n"
        "      catch ex: InvalidOperationException =>\n"
        "        total := total + 1;\n"
        "      end;\n"
        "      Program.Junk();\n"
        "      for var r in rows do total := total + r.Name.Length + r.N; end;\n"
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "4076\n");
}

// The array is an `owned` class field, read as `this.Rows[i]`, bare `Rows[i]`
// and `h.Rows[i]`: rotate, swap, then keep one and clear.
BEZEL_TEST(array_copy_out_local_coown_6566,
           receiver_is_an_owned_class_field_spec_28_11) {
    expectCleanRun(
        "module P;\n"
        "record E(Name: string, N: int);\n"
        "public class Holder\n"
        "  public owned Rows: Array<E> := new Array<E>();\n"
        "  public constructor() end;\n"
        "  public method Rot()\n"
        "    var t := this.Rows[0]; this.Rows.RemoveAt(0); this.Rows.Add(t);\n"
        "    var u := Rows[0]; Rows[0] := Rows[1]; Rows[1] := u;\n"
        "  end Rot;\n"
        "end Holder;\n"
        "public class Program\n"
        "  static method Junk()\n"
        "    var junk := new Array<string>();\n"
        "    for var k in 1..50 do junk.Add($\"junkjunkjunk{k}\"); end;\n"
        "  end Junk;\n"
        "  static method Build(i: int): owned Array<E>\n"
        "    var rows := new Array<E>();\n"
        "    var f := $\"name{i},zz{i},q{i}\".Split(',');\n"
        "    rows.Add(new E(f[0], 1)); rows.Add(new E(f[1], 2)); rows.Add(new E(f[2], 3));\n"
        "    return rows;\n"
        "  end Build;\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 1..200 do\n"
        "      using var h := new Holder();\n"
        "      var f := $\"name{i},zz{i},q{i}\".Split(',');\n"
        "      h.Rows.Add(new E(f[0], 1)); h.Rows.Add(new E(f[1], 2)); h.Rows.Add(new E(f[2], 3));\n"
        "      f.Clear();\n"
        "      h.Rot();\n"
        "      var t := h.Rows[2]; h.Rows.Clear(); h.Rows.Add(t);\n"
        "      Program.Junk();\n"
        "      for var r in h.Rows do total := total + r.Name.Length + r.N; end;\n"
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "1492\n");
}

// The copy is read through `rows.AsSpan()`; the span's backing array is the
// co-owner.
BEZEL_TEST(array_copy_out_local_coown_6566,
           copy_read_through_an_array_span_spec_28_11) {
    expectCleanRun(
        "module P;\n"
        "record E(Name: string, N: int);\n"
        "public class Program\n"
        "  static method Junk()\n"
        "    var junk := new Array<string>();\n"
        "    for var k in 1..50 do junk.Add($\"junkjunkjunk{k}\"); end;\n"
        "  end Junk;\n"
        "  static method Build(i: int): owned Array<E>\n"
        "    var rows := new Array<E>();\n"
        "    var f := $\"name{i},zz{i},q{i}\".Split(',');\n"
        "    rows.Add(new E(f[0], 1)); rows.Add(new E(f[1], 2)); rows.Add(new E(f[2], 3));\n"
        "    return rows;\n"
        "  end Build;\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 1..200 do\n"
        "      var rows := Build(i);\n"
        "      var sp := rows.AsSpan();\n"
        "      var t := sp[0];\n"
        "      rows.RemoveAt(0); rows.Add(t);\n"
        "      Program.Junk();\n"
        "      for var r in rows do total := total + r.Name.Length + r.N; end;\n"
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "4076\n");
}

// A generic record element, `Pair<string, int>`.
BEZEL_TEST(array_copy_out_local_coown_6566,
           generic_record_element_pair_of_string_int_spec_28_11) {
    expectCleanRun(
        "module P;\n"
        "record E(Name: string, N: int);\n"
        "record Pair<A, B>(First: A, Second: B);\n"
        "public class Program\n"
        "  static method Junk()\n"
        "    var junk := new Array<string>();\n"
        "    for var k in 1..50 do junk.Add($\"junkjunkjunk{k}\"); end;\n"
        "  end Junk;\n"
        "  static method Build(i: int): owned Array<E>\n"
        "    var rows := new Array<E>();\n"
        "    var f := $\"name{i},zz{i},q{i}\".Split(',');\n"
        "    rows.Add(new E(f[0], 1)); rows.Add(new E(f[1], 2)); rows.Add(new E(f[2], 3));\n"
        "    return rows;\n"
        "  end Build;\n"
        "  static method BuildP(i: int): owned Array<Pair<string, int>>\n"
        "    var rows := new Array<Pair<string, int>>();\n"
        "    var f := $\"name{i},zz{i}\".Split(',');\n"
        "    rows.Add(new Pair<string, int>(f[0], 1)); rows.Add(new Pair<string, int>(f[1], 2));\n"
        "    return rows;\n"
        "  end BuildP;\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 1..200 do\n"
        "      var rows := BuildP(i);\n"
        "      var t := rows[0]; rows[0] := rows[1]; rows[1] := t;\n"
        "      Program.Junk();\n"
        "      for var r in rows do total := total + r.First.Length + r.Second; end;\n"
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "2784\n");
}

// `DictionaryEntry<string, string>`: the copy holds the Key reference the
// array co-owns; swap and rotate, every Key and Value read back.
BEZEL_TEST(array_copy_out_local_coown_6566,
           entry_with_a_string_value_swap_and_rotate_spec_28_11) {
    expectCleanRun(
        "module M; class C\n"
        "  static method Junk()\n"
        "    var junk := new Array<string>();\n"
        "    for var k in 1..50 do junk.Add($\"junkjunkjunk{k}\"); end;\n"
        "  end Junk;\n"
        "  public static method Main(): int\n"
        "    var n := 0; var i := 0;\n"
        "    var d := new Dictionary<string, string>();\n"
        "    d.Add($\"a{n}\", $\"va{n}\"); d.Add($\"bb{n}\", $\"vbb{n}\"); d.Add(\"c\", \"vc\");\n"
        "    while i < 200 do\n"
        "      var es := new Array<DictionaryEntry<string, string>>();\n"
        "      for var e in d do es.Add(e); end for;\n"
        "      var t := es[0]; es[0] := es[1]; es[1] := t;\n"
        "      var u := es[2]; es.RemoveAt(2); es.Add(u);\n"
        "      C.Junk();\n"
        "      for var e in es do n := n + e.Key.Length + e.Value.Length; end;\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(n);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n",
        "3000\n");
}

// The local is re-assigned from a NON-indexer producer (`new E("lit", 9)`),
// stored, then re-bound from the indexer and rotated.
BEZEL_TEST(array_copy_out_local_coown_6566,
           local_reassigned_from_another_producer_then_rebound_spec_28_11) {
    expectCleanRun(
        "module P;\n"
        "record E(Name: string, N: int);\n"
        "public class Program\n"
        "  static method Junk()\n"
        "    var junk := new Array<string>();\n"
        "    for var k in 1..50 do junk.Add($\"junkjunkjunk{k}\"); end;\n"
        "  end Junk;\n"
        "  static method Build(i: int): owned Array<E>\n"
        "    var rows := new Array<E>();\n"
        "    var f := $\"name{i},zz{i},q{i}\".Split(',');\n"
        "    rows.Add(new E(f[0], 1)); rows.Add(new E(f[1], 2)); rows.Add(new E(f[2], 3));\n"
        "    return rows;\n"
        "  end Build;\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 1..200 do\n"
        "      var rows := Build(i);\n"
        "      var t := rows[0];\n"
        "      t := new E(\"lit\", 9);\n"
        "      rows[0] := t;\n"
        "      t := rows[1]; rows.RemoveAt(1); rows.Add(t);\n"
        "      Program.Junk();\n"
        "      for var r in rows do total := total + r.Name.Length + r.N; end;\n"
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "4984\n");
}

// `var t := rows[0]; rows.RemoveAt(0); return t.Name;` — the string read out
// of the copy survives the function's return.
BEZEL_TEST(array_copy_out_local_coown_6566,
           string_of_the_copy_returned_after_its_slot_was_removed_spec_28_11) {
    expectCleanRun(
        "module P;\n"
        "record E(Name: string, N: int);\n"
        "public class Program\n"
        "  static method Junk()\n"
        "    var junk := new Array<string>();\n"
        "    for var k in 1..50 do junk.Add($\"junkjunkjunk{k}\"); end;\n"
        "  end Junk;\n"
        "  static method Build(i: int): owned Array<E>\n"
        "    var rows := new Array<E>();\n"
        "    var f := $\"name{i},zz{i},q{i}\".Split(',');\n"
        "    rows.Add(new E(f[0], 1)); rows.Add(new E(f[1], 2)); rows.Add(new E(f[2], 3));\n"
        "    return rows;\n"
        "  end Build;\n"
        "  static method TakeName(rows: Array<E>): string\n"
        "    var t := rows[0]; rows.RemoveAt(0);\n"
        "    return t.Name;\n"
        "  end TakeName;\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 1..200 do\n"
        "      var rows := Build(i);\n"
        "      var s := Program.TakeName(rows);\n"
        "      Program.Junk();\n"
        "      total := total + s.Length;\n"
        "      for var r in rows do total := total + r.Name.Length + r.N; end;\n"
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "3876\n");
}

// PAIRING: copies bound in the loop body, in a conditional block and in an
// inner loop, never stored back. Nothing leaks, nothing is released twice.
BEZEL_TEST(array_copy_out_local_coown_6566,
           copies_bound_and_dropped_pair_every_take_with_a_release_spec_28_11) {
    expectCleanRun(
        "module P;\n"
        "record E(Name: string, N: int);\n"
        "public class Program\n"
        "  static method Junk()\n"
        "    var junk := new Array<string>();\n"
        "    for var k in 1..50 do junk.Add($\"junkjunkjunk{k}\"); end;\n"
        "  end Junk;\n"
        "  static method Build(i: int): owned Array<E>\n"
        "    var rows := new Array<E>();\n"
        "    var f := $\"name{i},zz{i},q{i}\".Split(',');\n"
        "    rows.Add(new E(f[0], 1)); rows.Add(new E(f[1], 2)); rows.Add(new E(f[2], 3));\n"
        "    return rows;\n"
        "  end Build;\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 1..200 do\n"
        "      var rows := Build(i);\n"
        "      var a := rows[0]; var b := rows[0]; var c := rows[0];\n"
        "      if i mod 2 = 0 then var d := rows[1]; total := total + d.N; end;\n"
        "      for var k in 0..2 do var e := rows[k]; total := total + e.Name.Length; end;\n"
        "      Program.Junk();\n"
        "      total := total + a.N + b.N + c.N;\n"
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "3676\n");
}

// A `string?` component (`EN(Name: string?, N: int)`), one slot holding null:
// swap, then rotate, every non-null Name read back.
BEZEL_TEST(array_copy_out_local_coown_6566,
           record_with_a_nullable_string_component_swap_and_rotate_spec_28_11) {
    expectCleanRun(
        "module P;\n"
        "record E(Name: string, N: int);\n"
        "record EN(Name: string?, N: int);\n"
        "public class Program\n"
        "  static method Junk()\n"
        "    var junk := new Array<string>();\n"
        "    for var k in 1..50 do junk.Add($\"junkjunkjunk{k}\"); end;\n"
        "  end Junk;\n"
        "  static method Build(i: int): owned Array<E>\n"
        "    var rows := new Array<E>();\n"
        "    var f := $\"name{i},zz{i},q{i}\".Split(',');\n"
        "    rows.Add(new E(f[0], 1)); rows.Add(new E(f[1], 2)); rows.Add(new E(f[2], 3));\n"
        "    return rows;\n"
        "  end Build;\n"
        "  static method BuildN(i: int): owned Array<EN>\n"
        "    var rows := new Array<EN>();\n"
        "    var f := $\"name{i},zz{i}\".Split(',');\n"
        "    rows.Add(new EN(f[0], 1)); rows.Add(new EN(null, 2)); rows.Add(new EN(f[1], 3));\n"
        "    return rows;\n"
        "  end BuildN;\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 1..200 do\n"
        "      var rows := BuildN(i);\n"
        "      var t := rows[0]; rows[0] := rows[1]; rows[1] := t;\n"
        "      var u := rows[2]; rows.RemoveAt(2); rows.Add(u);\n"
        "      Program.Junk();\n"
        "      for var r in rows do total := total + r.N + (r.Name ?? \"\").Length; end;\n"
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "3384\n");
}
