// #8592 — a co-owning array took a reference on EVERY string of every element
// stored into it, including strings that were already dead, so nine ordinary
// ways of carrying a record value to a store turned a program that ran clean
// into an abort.
//
// THE RULE (spec §28.9 "Runtime-managed types"; §28.11 "The element slot
// policy", operations table, copy-slot column; v0.357). `string` is
// runtime-managed: "the type's lifetime is managed by the runtime without
// user involvement" (§28.9). A value element's store "copies the value in",
// and its read is "copy out (a runtime-managed reference the copy holds — a
// `DictionaryEntry<string, V>`'s `Key` — keeps §28.9's unbounded lifetime
// through every later mutation or disposal of the collection)" (§28.11).
// Nothing in either section lets a store of a record value into an
// `Array<E>` fault — and nothing lets a string stored there die when the
// collection it was READ FROM is cleared.
//
// THE DEFECT. The implementation realizes "runtime-managed" by refcount. The
// three co-ownership fixes (#6557, #8386, #6566) made an `Array<E>` of
// string-carrying records hold one reference per embedded string per slot —
// and take it with an unconditional `bz_string_retain` at every store. But a
// record value is a byte copy that holds no reference of its own, so a value
// can reach the store carrying a string that is already freed — typically
// because the array itself was its last owner and gave it back one statement
// earlier:
//
//     var t := rows[0]; var u := t; t := rows[1];
//     rows[0] := rows[2];          // the array gives name{i} back: freed
//     rows[2] := u;                // retain on freed memory
//
// MEASURED on r30-c1 (`bezelc --sanitize=address`): each of the nine programs
// below — the issue's own pins, verbatim — dies with `AddressSanitizer: SEGV`
// inside `bz_string_retain`; the plain build prints `bezel: fatal: refcount
// over-release (string)`. Before the three fixes all nine ran clean and
// printed the totals asserted here. None of them ever reads the strings again.
//
// THE FIX. A co-owner takes its reference on every string it cannot PROVE
// dead. The proof is the runtime's own record: once a co-owner can exist, the
// final release of a string leaves its address in a set of TOMBSTONES and
// every string producer takes its address back out, so "in the set" means
// "freed, and no string has been created there since". A tombstoned string is
// stored as the bytes it is and the array remembers it holds no reference on
// it, so no later give-back releases one. A LIVE string is never in the set:
// whatever road the value took — a for-in variable, a dictionary value, a
// record parameter's component — the store takes its reference exactly as it
// did before this fix (the two `control_` rows; the first cut of this fix
// took a reference only on strings something vouched for and turned twelve
// such programs into a use-after-free).
//
// NOT IN THIS FIX: a program that READS a string after its last holder died
// (the same nine roads with `r.Name` read back) is a use-after-free before
// the three fixes, an abort after them, and a use-after-free again here — the
// residual #8484 tracks. This fix removes the abort; it does not invent a
// holder for a record local.
//
// ORACLE. Every row runs under `runAotChecked` — ASan plus strict
// LeakSanitizer — and asserts the exact stdout, that the program RAN TO
// COMPLETION, exit 0, and that nothing leaked or was over-released; then a
// second, plain `runAot` leg asserts rc=0 and the same stdout (the abort was
// observable without a sanitizer). Every row loops 200 times. The expected
// totals are computed from the program text, not read off the compiler.
//
// EVERY ROW BUT THE TWO NAMED `control_` ROWS IS RED ON THE BASE — measured,
// see the commit body. The other ten live-source programs of the review are
// spec-decided controls for the corpus, not rows here.

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

// The issue's programs. `Build(i)` returns a three-element `Array<E>` whose
// Names (`name{i}`, `zz{i}`, `q{i}`; N = 1, 2, 3) are read out of a `Split`
// result that dies when `Build` returns, so from then on the array is the only
// thing keeping them alive. `ops` carries an element to a store by one road;
// `Junk()` then reuses whatever was freed; `tail` reads the array back.
const char* const kReadN =
    "      for var r in rows do total := total + r.N; end;\n";

std::string reviewProgram(const std::string& ops, const char* tail = kReadN) {
    return std::string(
        "module P;\n"
        "record E(Name: string, N: int);\n"
        "record W(Inner: E, Tag: string);\n"
        "public class Holder\n"
        "  public R: E := new E(\"\", 0);\n"
        "  public constructor() end;\n"
        "end Holder;\n"
        "public class Program\n"
        "  static method Build(i: int): owned Array<E>\n"
        "    var rows := new Array<E>();\n"
        "    var f := $\"name{i},zz{i},q{i}\".Split(',');\n"
        "    rows.Add(new E(f[0], 1)); rows.Add(new E(f[1], 2)); rows.Add(new E(f[2], 3));\n"
        "    return rows;\n"
        "  end Build;\n"
        "  static method BuildW(i: int): owned Array<W>\n"
        "    var rows := new Array<W>();\n"
        "    var f := $\"wn{i},wz{i},wq{i}\".Split(',');\n"
        "    rows.Add(new W(new E(f[0], 1), f[1])); rows.Add(new W(new E(f[1], 2), f[2])); rows.Add(new W(new E(f[2], 3), f[0]));\n"
        "    return rows;\n"
        "  end BuildW;\n"
        "  static method Swap<T>(a: Array<T>, i: int, j: int)\n"
        "    var t := a[i]; a[i] := a[j]; a[j] := t;\n"
        "  end Swap;\n"
        "  static method SwapL(a: IList<E>, i: int, j: int)\n"
        "    var t := a[i]; a[i] := a[j]; a[j] := t;\n"
        "  end SwapL;\n"
        "  static method First(a: Array<E>): E\n"
        "    return a[0];\n"
        "  end First;\n"
        "  static method Junk()\n"
        "    var junk := new Array<string>();\n"
        "    for var k in 1..50 do junk.Add($\"junkjunkjunk{k}\"); end;\n"
        "  end Junk;\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 1..200 do\n"
        "      var rows := Build(i);\n") +
        ops +
        "      Program.Junk();\n" +
        tail +
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
}

// The three roads whose pins are whole programs of their own.
const char* const kBuildOnly =
    "module P;\n"
    "record E(Name: string, N: int);\n"
    "public class Program\n"
    "  static method Build(i: int): owned Array<E>\n"
    "    var rows := new Array<E>();\n"
    "    var f := $\"name{i},zz{i},q{i}\".Split(',');\n"
    "    rows.Add(new E(f[0], 1)); rows.Add(new E(f[1], 2)); rows.Add(new E(f[2], 3));\n"
    "    return rows;\n"
    "  end Build;\n";

const char* const kPopReadN =
    "      for var r in b do total := total + r.N; end;\n"
    "      for var r in rows do total := total + r.N; end;\n";

std::string popProgram(const std::string& ops, const char* tail = kPopReadN) {
    return std::string(
        "module P;\n"
        "record E(Name: string, N: int);\n"
        "public class Program\n"
        "  static method Junk()\n"
        "    var junk := new Array<string>();\n"
        "    for var k in 1..50 do junk.Add($\"junkjunkjunk{k}\"); end;\n"
        "  end Junk;\n"
        "  static method Pop(a: Array<E>): E\n"
        "    var t := a[a.Count - 1]; a.RemoveAt(a.Count - 1);\n"
        "    return t;\n"
        "  end Pop;\n"
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
        "      var b := new Array<E>();\n") +
        ops +
        tail +
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// The nine roads — the issue's pins, verbatim. Per iteration the array holds
// N = 1, 2, 3; a swap or a rotate keeps the sum at 6 (200 x 6 = 1200).
// ---------------------------------------------------------------------------

// r_local_to_local_n_only: `u` is a copy of the local `t`; `t` is re-bound, so
// nothing holds name{i} when `rows[0] := rows[2]` gives it back.
BEZEL_TEST(array_store_stale_record_string_8592,
           local_copied_to_a_second_local_then_stored_spec_28_11) {
    expectCleanBothLegs(reviewProgram(
        "      var t := rows[0]; var u := t; t := rows[1]; rows[0] := rows[2]; rows[2] := u;\n"),
        "1200\n");
}

// r_func_return_n_only: the element comes back from a method.
BEZEL_TEST(array_store_stale_record_string_8592,
           element_returned_by_a_method_then_stored_spec_28_11) {
    expectCleanBothLegs(reviewProgram(
        "      var t := Program.First(rows); rows[0] := rows[1]; rows[1] := t;\n"),
        "1200\n");
}

// r_first_linq_n_only: the element comes from `First()`.
BEZEL_TEST(array_store_stale_record_string_8592,
           element_from_linq_first_then_removed_and_added_spec_28_11) {
    expectCleanBothLegs(reviewProgram(
        "      var t := rows.First(); rows.RemoveAt(0); rows.Add(t);\n"),
        "1200\n");
}

// r_field_copy_n_only: the element is parked in a class field.
BEZEL_TEST(array_store_stale_record_string_8592,
           element_parked_in_a_class_field_then_stored_spec_28_11) {
    expectCleanBothLegs(reviewProgram(
        "      var h := new Holder(); h.R := rows[0]; rows[0] := rows[1]; rows[1] := h.R;\n"),
        "1200\n");
}

// r_forin_best_n_only: a for-in variable copied to an outer local; only the
// best (N = 3) survives the Clear: 200 x 3 = 600.
BEZEL_TEST(array_store_stale_record_string_8592,
           for_in_variable_copied_to_an_outer_local_then_added_spec_28_11) {
    expectCleanBothLegs(reviewProgram(
        "      var best := rows[0];\n"
        "      for var r in rows do if r.N > best.N then best := r; end; end;\n"
        "      rows.Clear(); rows.Add(best);\n"),
        "600\n");
}

// r_dead_local_add_n_only: `e` is built from a Split that has died by the time
// `e` is added: 6 + 7 per iteration, 200 x 13 = 2600.
BEZEL_TEST(array_store_stale_record_string_8592,
           local_built_from_a_dead_split_then_added_spec_28_9) {
    expectCleanBothLegs(reviewProgram(
        "      var e := new E(\"\", 0);\n"
        "      if true then var g := $\"dead{i},x\".Split(','); e := new E(g[0], 7); end;\n"
        "      Program.Junk(); rows.Add(e);\n"),
        "2600\n");
}

// r_pop_return_n_only: a pop-return (`var t := a[last]; a.RemoveAt(last);
// return t;`) added to a second array: b holds N = 3, rows 1 + 2.
BEZEL_TEST(array_store_stale_record_string_8592,
           element_popped_and_returned_then_added_spec_28_11) {
    expectCleanBothLegs(popProgram(
        "      b.Add(Program.Pop(rows)); Program.Junk();\n"),
        "1200\n");
}

// r_async_swap_n_only: the swap runs in an `async` body.
BEZEL_TEST(array_store_stale_record_string_8592,
           swap_through_a_local_in_an_async_body_spec_28_11) {
    expectCleanBothLegs(std::string(kBuildOnly) +
        "  static async method Work(i: int): Task<int>\n"
        "    using var rows := Build(i);\n"
        "    var t := rows[0]; rows[0] := rows[1]; rows[1] := t;\n"
        "    await Task.Delay(0);\n"
        "    var total := 0;\n"
        "    for var r in rows do total := total + r.N; end;\n"
        "    return total;\n"
        "  end Work;\n"
        "  public static async method Main(): Task<int>\n"
        "    var total := 0;\n"
        "    for var i in 1..200 do\n"
        "      total := total + await Work(i);\n"
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "1200\n");
}

// r_iter_swap_n_only: the swap runs in an iterator body.
BEZEL_TEST(array_store_stale_record_string_8592,
           swap_through_a_local_in_an_iterator_body_spec_28_11) {
    expectCleanBothLegs(std::string(kBuildOnly) +
        "  static method Gen(i: int): IEnumerable<int>\n"
        "    using var rows := Build(i);\n"
        "    var t := rows[0]; rows[0] := rows[1]; rows[1] := t;\n"
        "    for var r in rows do yield return r.N; end;\n"
        "  end Gen;\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 1..200 do\n"
        "      for var x in Gen(i) do total := total + x; end;\n"
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "1200\n");
}

// ---------------------------------------------------------------------------
// Give-backs after the roads. A slot stored WITHOUT a reference (the string
// was dead at the store: a class field and a returned value are byte copies
// that hold nothing) must never be given back as if it had one, and a slot
// stored WITH one must give back exactly one — through every way of losing a
// slot: RemoveAt, an overwrite, Clear, the array's own end.
// ---------------------------------------------------------------------------

// After the swap the array is [zz, name*, q] (* = stored without a reference).
// Add(h.R) -> [zz, name*, q, name*]; RemoveAt(1) -> [zz, q, name*];
// rows[0] := rows[2] -> [name*, q, name*]; RemoveAt(2) -> [name*, q].
// N: 1 + 3 = 4 per iteration, 200 x 4 = 800.
BEZEL_TEST(array_store_stale_record_string_8592,
           slot_stored_without_a_reference_is_removed_and_overwritten_spec_28_11) {
    expectCleanBothLegs(reviewProgram(
        "      var h := new Holder(); h.R := rows[0]; rows[0] := rows[1]; rows[1] := h.R;\n"
        "      rows.Add(h.R); rows.RemoveAt(1); rows[0] := rows[2]; rows.RemoveAt(2);\n"),
        "800\n");
}

// Three pop-returns: q (3), zz (2), name (1). b = [q] -> [q, zz] ->
// RemoveAt(0) -> [zz] -> Clear -> [] -> [name]; rows ends empty.
// N: 1 per iteration, 200 x 1 = 200.
BEZEL_TEST(array_store_stale_record_string_8592,
           popped_and_returned_elements_are_added_removed_and_cleared_spec_28_11) {
    expectCleanBothLegs(popProgram(
        "      b.Add(Program.Pop(rows)); b.Add(Program.Pop(rows)); b.RemoveAt(0); b.Clear();\n"
        "      b.Add(Program.Pop(rows)); Program.Junk();\n"),
        "200\n");
}

// ---------------------------------------------------------------------------
// Past the nine roads: the same dead string meeting the co-owner's other
// entry points. Each is red on the base at its first store.
// ---------------------------------------------------------------------------

// The string dies BEFORE any co-owning array has been created: `Stale(0)`
// returns a record whose Name was read out of a `Split` that died inside the
// callee, and the first `Array<E>` of the process stores it. Red on the base
// (the unconditional take writes into the freed string). It does NOT pin the
// module constructor that starts the record of deaths before `main`: with
// the constructor removed this row stays green, because the co-owner's take
// also refuses a string whose count reads zero and a freed header that no
// allocation has reused still reads zero. The constructor covers the case
// where the address HAS been reused by something that is not a string, which
// depends on the allocator and has no row. Each array holds N = 7 twice:
// 200 x 14 = 2800.
BEZEL_TEST(array_store_stale_record_string_8592,
           string_dead_before_the_first_co_owning_array_exists_spec_28_9) {
    expectCleanBothLegs(R"BZL(module P;
record E(Name: string, N: int);
public class Program
  static method Junk()
    var junk := new Array<string>();
    for var k in 1..50 do junk.Add($"junkjunkjunk{k}"); end;
  end Junk;
  static method Stale(i: int): E
    var e := new E("", 0);
    if true then var g := $"dead{i},x".Split(','); e := new E(g[0], 7); end;
    return e;
  end Stale;
  public static method Main(): int
    var total := 0;
    var e := Program.Stale(0);
    for var i in 1..200 do
      var rows := new Array<E>();
      rows.Add(e); rows.Add(Program.Stale(i));
      Program.Junk();
      for var r in rows do total := total + r.N; end;
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
)BZL",
        "2800\n");
}

// A slot that carries a dead string is copied out into a local (`var w :=
// rows[2]`: the local's holder must take nothing on it and give nothing
// back) and stored again. After the rotate the array is [q, zz, name*];
// w = name* (N = 1); rows[1] := w -> [q, name*, name*]: 1 + 3 + 1 + 1 = 6 per
// iteration, 200 x 6 = 1200.
BEZEL_TEST(array_store_stale_record_string_8592,
           slot_carrying_a_dead_string_is_copied_out_and_stored_again_spec_28_11) {
    expectCleanBothLegs(reviewProgram(
        "      var t := rows[0]; var u := t; t := rows[1]; rows[0] := rows[2]; rows[2] := u;\n"
        "      Program.Junk();\n"
        "      var w := rows[2];\n"
        "      total := total + w.N;\n"
        "      rows[1] := w;\n"),
        "1200\n");
}

// The same slot leaves through `return rows[2]` of an array that dies with
// the returning method (#8627's copy-out): the returned value takes nothing
// on the dead string. N = 1 per call: 200 x 1 = 200.
BEZEL_TEST(array_store_stale_record_string_8592,
           slot_carrying_a_dead_string_is_returned_out_of_a_dying_array_spec_28_11) {
    expectCleanBothLegs(std::string(kBuildOnly) +
        "  static method Junk()\n"
        "    var junk := new Array<string>();\n"
        "    for var k in 1..50 do junk.Add($\"junkjunkjunk{k}\"); end;\n"
        "  end Junk;\n"
        "  static method LastOf(i: int): E\n"
        "    var rows := Build(i);\n"
        "    var t := rows[0]; var u := t; t := rows[1]; rows[0] := rows[2]; rows[2] := u;\n"
        "    Program.Junk();\n"
        "    return rows[2];\n"
        "  end LastOf;\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 1..200 do\n"
        "      total := total + Program.LastOf(i).N;\n"
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "200\n");
}

// ---------------------------------------------------------------------------
// The two named controls (green on the base). The stored string is ALIVE, and
// nothing the compiler can see at the store says so; its source is emptied
// afterwards and the Names are read back, so the array must have taken its
// own reference (§28.11: the stored copy "keeps §28.9's unbounded lifetime
// through every later mutation or disposal of the collection").
//
// THE WRONG FIX BOTH CATCH: "take a reference only when the source is known
// to be alive" (a static vouch for the stored expression, or a registry of
// strings some co-owner already holds) — the first cut of this fix, which
// stored these strings unowned: heap-use-after-free at `r.Name.Length`.
// THE SECOND WRONG FIX BOTH CATCH: tombstones that are never taken back out
// when a new string is created at the same address — from the second
// iteration on, the fresh strings land on tombstoned addresses and would be
// stored unowned.
// ---------------------------------------------------------------------------

// A for-in variable over an `Array<string>` that is then cleared. Per
// iteration len(fa{i}) + len(fb{i}) + 2 = 6 + 2d; 200 x 6 + 2 x 492 = 2184
// (492 = the digit count of 1..200: 9 + 180 + 303).
BEZEL_TEST(array_store_stale_record_string_8592,
           control_for_in_string_stored_then_its_source_cleared_spec_28_11) {
    expectCleanBothLegs(R"BZL(module P;
record E(Name: string, N: int);
public class Program
  public static method Main(): int
    var total := 0;
    for var i in 1..200 do
      var rows := new Array<E>();
      var strs := $"fa{i},fb{i}".Split(',');
      for var s in strs do rows.Add(new E(s, 1)); end;
      strs.Clear();
      for var r in rows do total := total + r.Name.Length + r.N; end;
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
)BZL",
        "2184\n");
}

// A record VALUE read out of a `Dictionary<int, E>` whose entry is then
// removed: the stored value is a byte copy that came from another container,
// the one shape for which "a byte copy holds nothing, so take nothing from
// it" looks most plausible. Per iteration len(dv{i}) + 1 = 3 + d;
// 200 x 3 + 492 = 1092.
BEZEL_TEST(array_store_stale_record_string_8592,
           control_dictionary_record_value_stored_then_its_entry_removed_spec_28_11) {
    expectCleanBothLegs(R"BZL(module P;
record E(Name: string, N: int);
public class Program
  static method MakeDict(i: int): owned Dictionary<int, E>
    var d := new Dictionary<int, E>();
    d.Add(1, new E($"dv{i}", 1));
    return d;
  end MakeDict;
  public static method Main(): int
    var total := 0;
    for var i in 1..200 do
      var rows := new Array<E>();
      var d := Program.MakeDict(i);
      rows.Add(d[1]);
      d.Remove(1);
      for var r in rows do total := total + r.Name.Length + r.N; end;
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
)BZL",
        "1092\n");
}
