// #8386 — `rows.Add(new E(f[0]))`, with `f` a loop-local `Array<string>`:
// the record's string component was stored in `rows` BORROWED, so it died with
// `f` and every later `rows[i].Name` read freed memory.
//
// THE RULE (spec §28.9 "Runtime-managed types", §28.3.1 K1.6, §28.11; v0.357).
// `string` is runtime-managed (§28.9), and K1.6 says a value of runtime-managed
// type carries "no classification and no obligation": the program neither
// disposes a string nor has to keep the place it was read from alive. A record
// copies its components (§9), and §28.11's value row makes `Array<E>.Add` a
// value copy of the record. Nothing in the program disposes the string, so the
// copy stored in `rows` must read back as it was stored for as long as `rows`
// holds it.
//
// THE DEFECT. The implementation realizes "runtime-managed" by refcount. A
// record value's string component carries whatever reference state it had at
// construction — for `new E(f[0])` a BORROW of `f`'s element — and `Add` was a
// byte copy that took no reference. When `f` was freed at the end of the
// iteration the string went with it:
//
//     for var i in 1..3 do
//       var f := $"name{i},z".Split(',');
//       rows.Add(new E(f[0]));
//     end;
//     ... for var r in rows do Console.WriteLine(r.Name); end;
//
// MEASURED on the base (`0b30832a7`): the unchecked build printed
// `junkjunkjunk1` (whatever reused the block) and exited 0; under
// `--sanitize=address` the first `r.Name` read crashed.
//
// THE FIX. `new Array<E>(...)`, for a record `E` with string components, stamps
// the array at construction as the CO-OWNER of every string its slots embed
// (`bz_array_own_embedded_strings`) — the #6550 / #6557 discipline, generalized
// from "the Key at field 0" to "every string the element carries" by two
// compiler-generated per-element-type thunks. `Add` / `Insert` take, the
// indexer store and the span stores take-then-release, `RemoveAt` / `Clear` /
// the frees give back.
//
// ORACLE. Every row runs under `runAotChecked` — ASan plus strict LeakSanitizer
// — and asserts the exact stdout, that the program RAN TO COMPLETION, and that
// nothing leaked or was over-released. The use-after-free is the crash half
// (`r.ran`) and the stdout half (a reused block prints the wrong text); a fix
// that retained without releasing would pass those and fail the leak half.
// Every row but the literal repro loops 100 times. The expected totals are
// computed from the program text, not read off the compiler.
//
// NOT COVERED HERE, and deliberately not asserted (each measured, none made
// worse by this fix; see the commit body):
//   - `rows.Add(new E($"..."))` and `var n := f[0]; rows.Add(new E(n));` print
//     correctly but LEAK the string, before and after (#6153 family);
//   - a record LOCAL copied out of the array is a borrow of the slot (#6566);
//   - `Stack<E>` / `Queue<E>`, a record-typed class field, an `Array<E?>`
//     and an `Array<E>` built by a collection literal are not stamped (#8484).

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::JitResult;
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

// One iteration of the mutation rows. `Build(i)` returns a three-element
// `Array<E>` whose Names (`name{i}`, `zz{i}`, `q{i}`; N = 1, 2, 3) are read out
// of a `Split` result that dies when `Build` returns, so from then on the
// array is the only thing keeping them alive. `ops` then mutates `rows`, and
// every Name the array still holds is read back. `Put(rows, i)` adds
// `E(name{i}, i)` the same way through a parameter.
std::string recordProgram(const std::string& ops) {
    return std::string(
        "module P;\n"
        "record E(Name: string, N: int);\n"
        "public class Src\n"
        "  public S: string := \"\";\n"
        "  public constructor(n: string) this.S := n; end;\n"
        "end Src;\n"
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
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 1..100 do\n"
        "      var rows := Build(i);\n")
        + "      " + ops + "\n" +
        "      for var r in rows do total := total + r.Name.Length + r.N; end;\n"
        "      total := total + rows.Count;\n"
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// THE ISSUE'S OWN REPRO, verbatim.

BEZEL_TEST(array_record_add_string_coown_8386,
           issue_repro_add_record_of_indexer_read_spec_28_9) {
    expectCleanRun(
        "module P;\n"
        "record E(Name: string);\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var rows := new Array<E>();\n"
        "    for var i in 1..3 do\n"
        "      var f := $\"name{i},z\".Split(',');\n"
        "      rows.Add(new E(f[0]));\n"
        "    end;\n"
        "    var junk := new Array<string>();\n"
        "    for var i in 1..100 do junk.Add($\"junkjunkjunk{i}\"); end;\n"
        "    for var r in rows do Console.WriteLine(r.Name); end;\n"
        "  end Main;\n"
        "end Program;\n",
        "name1\nname2\nname3\n");
}

// ---------------------------------------------------------------------------
// The mutators, 100 iterations each.

// The array is built inside `Build` from the elements of a `Split` result
// that dies when `Build` returns: every string the caller reads is one only
// the array kept alive.
BEZEL_TEST(array_record_add_string_coown_8386,
           returned_array_outlives_its_source_spec_28_9) {
    expectCleanRun(recordProgram(
            " \n"),
        "2176\n");
}

// `Add` through a parameter: the stamp travels with the array, not with the
// static type at the `Add` site.
BEZEL_TEST(array_record_add_string_coown_8386,
           add_through_a_parameter_spec_28_9) {
    expectCleanRun(recordProgram(
            "Put(rows, i); Put(rows, i + 1);\n"),
        "13762\n");
}

// `Insert` takes the incoming element's strings; the shifted tail moves no
// reference.
BEZEL_TEST(array_record_add_string_coown_8386,
           insert_takes_the_strings_spec_28_11) {
    expectCleanRun(recordProgram(
            "if true then var g := $\"ins{i},x\".Split(','); rows.Insert(1, new E(g[0], 9)); end;\n"),
        "3668\n");
}

// The indexer store takes the incoming element's strings and gives the
// displaced element's back (a leak if it only took; a use-after-free if it
// only gave back).
BEZEL_TEST(array_record_add_string_coown_8386,
           indexer_store_takes_incoming_releases_displaced_spec_28_11) {
    expectCleanRun(recordProgram(
            "if true then var g := $\"set{i},x\".Split(','); rows[1] := new E(g[0], 9); end;\n"),
        "2976\n");
}

// `rows[k] := rows[k]`: take-then-release, so the slot's own string is never
// freed under it.
BEZEL_TEST(array_record_add_string_coown_8386,
           indexer_self_store_is_the_identity_spec_28_11) {
    expectCleanRun(recordProgram(
            "rows[1] := rows[1]; rows[0] := rows[0];\n"),
        "2176\n");
}

// Two slots end up sharing one string: two references, one per slot.
BEZEL_TEST(array_record_add_string_coown_8386,
           indexer_store_of_another_slot_spec_28_11) {
    expectCleanRun(recordProgram(
            "rows[0] := rows[2];\n"),
        "2076\n");
}

// `RemoveAt` gives back the removed slot's strings and only those.
BEZEL_TEST(array_record_add_string_coown_8386,
           remove_at_releases_only_the_removed_slot_spec_28_11) {
    expectCleanRun(recordProgram(
            "rows.RemoveAt(1);\n"),
        "1484\n");
}

// `Clear` gives every string back and the array still owns what is added
// after it.
BEZEL_TEST(array_record_add_string_coown_8386,
           clear_then_refill_keeps_owning_spec_28_11) {
    expectCleanRun(recordProgram(
            "rows.Clear(); Put(rows, i); Put(rows, i);\n"),
        "11484\n");
}

// `new Array<E>(n, fill)`: the fill value's string is shared by n slots, so
// the array holds n references on it.
BEZEL_TEST(array_record_add_string_coown_8386,
           size_fill_constructor_owns_every_filled_slot_spec_28_11) {
    expectCleanRun(recordProgram(
            "if true then var g := $\"fill{i},x\".Split(','); rows := new Array<E>(3, new E(g[0], 2)); end;\n"),
        "2676\n");
}

// A span `Fill` is n indexer stores on the backing array.
BEZEL_TEST(array_record_add_string_coown_8386,
           span_fill_takes_per_slot_spec_4_15) {
    expectCleanRun(recordProgram(
            "if true then var g := $\"fill{i},x\".Split(','); rows.AsSpan().Fill(new E(g[0], 2)); end;\n"),
        "2676\n");
}

// A span `CopyTo` between two record arrays: each destination slot takes the
// incoming strings and gives its old ones back.
BEZEL_TEST(array_record_add_string_coown_8386,
           span_copy_to_takes_incoming_releases_displaced_spec_4_15) {
    expectCleanRun(recordProgram(
            "var other := Build(i + 1); other.AsSpan().CopyTo(rows.AsSpan());\n"),
        "2182\n");
}

// PRODUCER: a field read (`src.S`), the source object disposed at the end of
// the block.
BEZEL_TEST(array_record_add_string_coown_8386,
           field_read_producer_spec_28_9) {
    expectCleanRun(recordProgram(
            "if true then using var src := new Src($\"fld{i}\"); rows.Add(new E(src.S, 4)); end;\n"),
        "3168\n");
}

// PRODUCER: a record LOCAL built from an indexer read, added, then its source
// dies. (The local itself is not read after that.)
BEZEL_TEST(array_record_add_string_coown_8386,
           record_local_added_then_source_dies_spec_28_9) {
    expectCleanRun(recordProgram(
            "if true then var g := $\"loc{i},x\".Split(','); var e := new E(g[0], 5); rows.Add(e); end;\n"),
        "3268\n");
}

// `AddRange` from a second record array that then dies.
BEZEL_TEST(array_record_add_string_coown_8386,
           add_range_takes_every_element_spec_28_11) {
    expectCleanRun(recordProgram(
            "var other := Build(i + 1); rows.AddRange(other);\n"),
        "4358\n");
}

// In-place `Sort` / `Reverse` (array and span) permute slots: no reference
// moves, and nothing is released twice at free.
BEZEL_TEST(array_record_add_string_coown_8386,
           sort_and_reverse_are_permutations_spec_28_11) {
    expectCleanRun(recordProgram(
            "rows.Sort(lambda(x, y) => y.N - x.N); rows.Reverse(); rows.AsSpan().Reverse();\n"),
        "2176\n");
}

// ---------------------------------------------------------------------------
// Element shapes: where the strings sit in the element.

// Two string components (`Pair`), a nested record plus a trailing string
// (`Outer`), and a generic record instantiated at `string` (`Wrap<string>`),
// each array mutated every tenth iteration and all read after every source
// `f` has died.
BEZEL_TEST(array_record_add_string_coown_8386,
           multi_string_nested_and_generic_record_elements_spec_28_9) {
    expectCleanRun(
        "module P;\n"
        "record Pair(A: string, B: string);\n"
        "record Outer(Tag: int, In: Pair, S: string);\n"
        "record Wrap<T>(V: T);\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    var ps := new Array<Pair>();\n"
        "    var os := new Array<Outer>();\n"
        "    var ws := new Array<Wrap<string>>();\n"
        "    for var i in 1..100 do\n"
        "      var f := $\"aa{i},b{i},cccc{i}\".Split(',');\n"
        "      ps.Add(new Pair(f[0], f[1]));\n"
        "      os.Add(new Outer(i, new Pair(f[1], f[2]), f[0]));\n"
        "      ws.Add(new Wrap<string>(f[2]));\n"
        "      if i mod 10 = 0 then\n"
        "        ps.RemoveAt(0);\n"
        "        os[0] := os[os.Count - 1];\n"
        "        ws.Insert(0, new Wrap<string>(f[0]));\n"
        "      end;\n"
        "    end;\n"
        "    for var p in ps do total := total + p.A.Length + p.B.Length; end;\n"
        "    for var o in os do total := total + o.In.A.Length + o.In.B.Length + o.S.Length + o.Tag; end;\n"
        "    for var w in ws do total := total + w.V.Length; end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "7696\n");
}

// A `string?` component: a live string through `?.`, a null through `?.`, and a
// literal null. The thunks must take the first and tolerate the other two.
BEZEL_TEST(array_record_add_string_coown_8386,
           nullable_string_component_through_null_conditional_spec_28_9) {
    expectCleanRun(
        "module P;\n"
        "record E(Name: string?, N: int);\n"
        "public class Src\n"
        "  public S: string := \"\";\n"
        "  public constructor(n: string) this.S := n; end;\n"
        "end Src;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    var rows := new Array<E>();\n"
        "    for var i in 1..100 do\n"
        "      using var src := new Src($\"name{i}\");\n"
        "      var m: Src? := src;\n"
        "      var none: Src? := null;\n"
        "      rows.Add(new E(m?.S, i));\n"
        "      rows.Add(new E(none?.S, i));\n"
        "      rows.Add(new E(null, 1));\n"
        "    end;\n"
        "    for var r in rows do total := total + (r.Name?.Length ?? 0) + r.N; end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "10792\n");
}

// The array is constructed inside a generic class (`new Array<T>()` in
// `Bag<T>`) and used at `T = E`.
BEZEL_TEST(array_record_add_string_coown_8386,
           array_constructed_in_generic_class_at_record_spec_28_9) {
    expectCleanRun(
        "module P;\n"
        "record E(Name: string, N: int);\n"
        "public class Bag<T>\n"
        "  public owned Items: Array<T> := new Array<T>();\n"
        "  public method Put(x: T)  Items.Add(x);  end Put;\n"
        "end Bag;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    var bag := new Bag<E>();\n"
        "    for var i in 1..100 do\n"
        "      var f := $\"name{i},z\".Split(',');\n"
        "      bag.Put(new E(f[0], i));\n"
        "    end;\n"
        "    for var r in bag.Items do total := total + r.Name.Length + r.N; end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "5642\n");
}

// ---------------------------------------------------------------------------
// THE GATE'S NEGATIVE SPACE: a record with no string component is not stamped
// and behaves exactly as before under the same mutators.
BEZEL_TEST(array_record_add_string_coown_8386,
           control_record_without_strings_is_untouched_spec_28_11) {
    expectCleanRun(
        "module P;\n"
        "record P2(X: int, Y: int);\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    for var i in 1..100 do\n"
        "      var ps := new Array<P2>();\n"
        "      ps.Add(new P2(i, 1)); ps.Add(new P2(i, 2)); ps.Insert(0, new P2(3, 3));\n"
        "      ps[1] := ps[2]; ps.RemoveAt(0); ps.AsSpan().Fill(new P2(i, 5));\n"
        "      for var p in ps do total := total + p.X + p.Y; end;\n"
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        "11100\n");
}
