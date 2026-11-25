// Codegen / collections — a key READ OUT OF a generic class's dictionary did not
// match the stored key, so `Remove` with it returned false and kept the entry
// (stdlib §4.8). Issue #9050.
//
// THE DEFECT. `Dictionary<K, V>`'s backing is chosen from the key type: a
// `string` key keeps the dedicated string fast path (`bz_dict_alloc` +
// `bz_dict_*_str`), every other hashable key uses the generic hash table
// (`bz_dict_alloc_generic` + `bz_dict_*_gen`). Inside a MONOMORPHIZED generic
// body the key type PARAMETER is still the PLACEHOLDER (`K`, a Named TypeRef
// with a null declaration); it resolves to its concrete substitution only at
// the instantiation (§14.1 *Instantiation and its site*, §3.6 rule 2). The
// codegen sites that pick the table's backing resolved the parameter
// (`genericBackingType`), but the sites that decided "is this key a STRING?"
// read the unresolved placeholder — so the two disagreed at `K = string`: the
// snapshot was taken in the STRING shape (`bz_dict_keys_str`) and the entry
// was decoded in the GENERIC branch. The decoded key was a NULL pointer, the
// snapshot's per-key bytes were never freed (a leak), and a key read out of
// the dictionary matched nothing: `Remove` returned false, `ContainsKey` /
// `TryGetValue` / the indexer all missed it, and printing it faulted.
//
// THE FIX. The string-vs-generic classification reads the RESOLVED key type at
// every site that pairs it with the table's backing, so the snapshot and the
// decode always agree on the key's shape.
//
// Spec: §4.8 *`Dictionary<K, V>`* (the key's hash/equality contract; "`Remove`
// ... returns `true` iff it removed an element"), §3.6 rule 2 *Derived type
// properties* (a generic's properties are read at each instantiation, AFTER
// substituting the type arguments), §14.1 *Instantiation and its site*, §4.6
// *`string`* (content equality), §28.9 (runtime-managed types). Spec revision
// v0.359.
//
// The oracle: `runAot` for the value rows (stdout + exit code) and
// `runAotChecked` for the leak row (200 iterations — the snapshot's per-key
// bytes and its length/width spines are freed only when the string branch
// runs). The `Select` projection row runs on `runAot`: the LINQ materializer's
// string snapshot leaks its length/width spines in the NON-generic case too
// (a pre-existing, separately-tracked defect), so a leak-checked oracle would
// report that leak rather than this issue's.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The issue's own repro, with the key read out of the dictionary and handed
// straight back to `Remove` (the issue's spelling copies it through an
// `Array<K>` first; that round-trip additionally exercises the generic
// `Array<T>`'s string-element reference, issue #9100, which is a separate
// defect and is not what this issue is about).
const char* kRemoveWithTheKeyReadOut =
    "module M; import System;\n"
    "public class Bag<K> where K: hashable\n"
    "  private owned _d: Dictionary<K, int> := new Dictionary<K, int>();\n"
    "  public method Put(k: K, v: int) => _d[k] := v;\n"
    "  public method DropFirst(): int\n"
    "    var removed := false;\n"
    "    for var kv in _d do removed := _d.Remove(kv.Key); break; end;\n"
    "    Console.WriteLine($\"removed {removed}\");\n"
    "    return _d.Count;\n"
    "  end DropFirst;\n"
    "end Bag;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    using var b := new Bag<string>();\n"
    "    b.Put(\"a\", 1); b.Put(\"b\", 9);\n"
    "    Console.WriteLine(b.DropFirst());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The key read out of the dictionary IS the stored key: it prints as the key
// that was put in, not as an empty (null) string.
const char* kTheKeyReadOut =
    "module M; import System;\n"
    "public class Bag<K> where K: hashable\n"
    "  private owned _d: Dictionary<K, int> := new Dictionary<K, int>();\n"
    "  public method Put(k: K, v: int) => _d[k] := v;\n"
    "  public method Probe()\n"
    "    for var kv in _d do Console.WriteLine(kv.Key); break; end;\n"
    "  end Probe;\n"
    "end Bag;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    using var b := new Bag<string>();\n"
    "    b.Put(\"a\", 1);\n"
    "    b.Probe();\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The other key readers of the same key: `ContainsKey`, `TryGetValue` and the
// indexer all see it.
const char* kTheLookups =
    "module M; import System;\n"
    "public class Bag<K> where K: hashable\n"
    "  private owned _d: Dictionary<K, int> := new Dictionary<K, int>();\n"
    "  public method Put(k: K, v: int) => _d[k] := v;\n"
    "  public method Probe()\n"
    "    for var kv in _d do\n"
    "      Console.WriteLine($\"contains {_d.ContainsKey(kv.Key)}\");\n"
    "      var v: int? := null;\n"
    "      Console.WriteLine($\"tryget {_d.TryGetValue(kv.Key, out v)}\");\n"
    "      Console.WriteLine($\"indexer {_d[kv.Key]}\");\n"
    "      break;\n"
    "    end;\n"
    "  end Probe;\n"
    "end Bag;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    using var b := new Bag<string>();\n"
    "    b.Put(\"a\", 1);\n"
    "    b.Probe();\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The LINQ projection of the key — the spelling the issue's study reported as
// faulting when the projected key was printed.
const char* kTheProjectedKey =
    "module M; import System;\n"
    "public class Bag<K> where K: hashable\n"
    "  private owned _d: Dictionary<K, int> := new Dictionary<K, int>();\n"
    "  public method Put(k: K, v: int) => _d[k] := v;\n"
    "  public method Project(): Array<K>\n"
    "    return _d.Select(lambda(kv) => kv.Key).ToArray();\n"
    "  end Project;\n"
    "end Bag;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    using var b := new Bag<string>();\n"
    "    b.Put(\"a\", 1);\n"
    "    var arr := b.Project();\n"
    "    Console.WriteLine($\"count {arr.Count}\");\n"
    "    Console.WriteLine($\"[{arr[0]}]\");\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The leak row: 200 iterations of the entry iteration. Each iteration took a
// key snapshot whose per-key bytes and whose length/width spines were never
// freed (the string branch of the decode is what frees them), so the program
// leaked on every iteration. `runAotChecked` runs under ASan/LSan, so the
// leak (the bug) flips `r.ran`; a use-after-free or double-free would too.
const char* kTheSnapshotIsReleased =
    "module M; import System;\n"
    "public class Bag<K> where K: hashable\n"
    "  private owned _d: Dictionary<K, int> := new Dictionary<K, int>();\n"
    "  public method Put(k: K, v: int) => _d[k] := v;\n"
    "  public method DropFirst(): int\n"
    "    var removed := false;\n"
    "    for var kv in _d do removed := _d.Remove(kv.Key); break; end;\n"
    "    return _d.Count;\n"
    "  end DropFirst;\n"
    "end Bag;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var n := 0;\n"
    "    var i := 0;\n"
    "    while i < 200 do\n"
    "      using var b := new Bag<string>();\n"
    "      b.Put(\"a\", 1); b.Put(\"b\", 9);\n"
    "      n := n + b.DropFirst();\n"
    "      i := i + 1;\n"
    "    end;\n"
    "    Console.WriteLine(n);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// CONTROL (the issue's own twin): the same class at `K = int` was already
// correct — the `int` key never took the string fast path, so the snapshot and
// the decode agreed on the generic shape.
const char* kControlTheIntTwin =
    "module M; import System;\n"
    "public class Bag<K> where K: hashable\n"
    "  private owned _d: Dictionary<K, int> := new Dictionary<K, int>();\n"
    "  public method Put(k: K, v: int) => _d[k] := v;\n"
    "  public method DropFirst(): int\n"
    "    var removed := false;\n"
    "    for var kv in _d do removed := _d.Remove(kv.Key); break; end;\n"
    "    Console.WriteLine($\"removed {removed}\");\n"
    "    return _d.Count;\n"
    "  end DropFirst;\n"
    "end Bag;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    using var c := new Bag<int>();\n"
    "    c.Put(1, 1); c.Put(2, 9);\n"
    "    Console.WriteLine(c.DropFirst());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// CONTROL: the non-generic `Dictionary<string, int>` reads its key the same
// way and was already correct — its key type is a concrete `string`, so the
// two classifications could not disagree.
const char* kControlTheNonGenericTwin =
    "module M; import System;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var d := new Dictionary<string, int>();\n"
    "    d[\"a\"] := 1; d[\"b\"] := 9;\n"
    "    var removed := false;\n"
    "    for var kv in d do removed := d.Remove(kv.Key); break; end;\n"
    "    Console.WriteLine($\"removed {removed}\");\n"
    "    Console.WriteLine(d.Count);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

void expectOut(const char* src, const char* out) {
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

}  // namespace

BEZEL_TEST(ownership_dict_generic_string_key_read_out_9050,
           remove_with_the_key_read_out_of_the_dictionary_spec_4_8) {
    expectOut(kRemoveWithTheKeyReadOut, "removed True\n1\n");
}

BEZEL_TEST(ownership_dict_generic_string_key_read_out_9050,
           the_key_read_out_is_the_stored_key_spec_4_8) {
    expectOut(kTheKeyReadOut, "a\n");
}

BEZEL_TEST(ownership_dict_generic_string_key_read_out_9050,
           containskey_trygetvalue_and_the_indexer_see_that_key_spec_4_8) {
    expectOut(kTheLookups, "contains True\ntryget True\nindexer 1\n");
}

BEZEL_TEST(ownership_dict_generic_string_key_read_out_9050,
           the_projected_key_of_a_select_spec_4_8) {
    expectOut(kTheProjectedKey, "count 1\n[a]\n");
}

BEZEL_TEST(ownership_dict_generic_string_key_read_out_9050,
           the_key_snapshot_is_released_spec_28_9) {
    JitResult r = runAotChecked(kTheSnapshotIsReleased);
    BEZEL_EXPECT_TRUE(r.ran);          // no leak (the bug), no double-free
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n"));
}

BEZEL_TEST(ownership_dict_generic_string_key_read_out_9050,
           control_the_int_twin_is_unchanged_spec_4_8) {
    expectOut(kControlTheIntTwin, "removed True\n1\n");
}

BEZEL_TEST(ownership_dict_generic_string_key_read_out_9050,
           control_the_non_generic_twin_is_unchanged_spec_4_8) {
    expectOut(kControlTheNonGenericTwin, "removed True\n1\n");
}
