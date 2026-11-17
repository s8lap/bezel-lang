// Spec-derived regression test for issue #7076 (narrow: the scope-end release
// of a hash-backed dictionary quarantines in checked disposal mode).
//
// §28.2 (use after disposal; checked disposal mode) and §28.3 (the store-rule
// residual), v0.357.
//
// THE RULE. §28.2: in checked disposal mode "any instance member access on a
// logically-disposed instance ... raises `DisposedAccessException`", and an
// implementation may "defer reclaiming a disposed instance's memory in checked
// mode (quarantine, delayed free) so that the flag remains readable after
// disposal". §28.3's store-rule residual admits a call that stores a borrow of
// a plain argument in a field; a collection-literal argument is a temporary
// disposed at its boundary, so a later access through the stored borrow is an
// access to a disposed instance.
//
// THE DEFECT. The scope-end release of a dictionary (`emitDisposals`, the
// `it->dict` arm) called `bz_dict_free` in checked mode too, reclaiming the
// header. The method-entry and `.Count` guards then read `bz_dict_is_disposed`
// out of freed memory, saw "live", and went on to walk a freed bucket array:
// measured SIGSEGV (exit 139) on the member-call shapes below and a garbage
// integer on the `.Count` ones, in checked mode. #7045
// fixed exactly this for the array-backed collections; the dictionary arm was
// left for #7076. The release now quarantines (`bz_dict_mark_disposed`) in
// checked mode, so the guard reads a real flag and the access faults.
//
// THE ORACLE. Each stale-borrow row builds the holder 100 times and expects
// 100 `faulted` lines: one iteration proves little about a read of freed
// memory, whose outcome depends on what the allocator did with the block. The
// two boundary controls prove what the quarantine must NOT change: the value
// cascade still runs exactly once per element, a live dictionary still
// answers, the retained headers are not LeakSanitizer leaks, and unchecked
// mode still frees. Each names the wrong fix that turns it red.
//
// The same commit arms the `.Count` guard for a dict-backed `Set<T>` (it was
// skipped): with the header quarantined, an unguarded `.Count` would answer 0
// from the reset table — the "reset empty" outcome the #7045 ruling rejects.
//
// OUT OF SCOPE (still #7076): a `for ... in` enumeration and a dictionary
// indexer read through the stored borrow are not guarded by the flag.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedDisposal;

namespace {

const char* kDidNotRun = "<did-not-run>";

// §28.2 checked disposal mode, plain build.
std::string outChecked(const std::string& src) {
    auto r = runAotCheckedDisposal(src);
    return r.ran ? r.stdoutText : std::string(kDidNotRun);
}

// Checked disposal mode under generated-code AddressSanitizer + LeakSanitizer.
std::string outCheckedSan(const std::string& src) {
    bezel::codegen::CodegenOptions opts;
    opts.checkedDisposal = true;
    auto r = runAotChecked(src, opts);
    return r.ran ? r.stdoutText : std::string(kDidNotRun);
}

// Unchecked mode under AddressSanitizer + LeakSanitizer: the immediate free
// stays, so nothing may be retained.
std::string outUncheckedSan(const std::string& src) {
    bezel::codegen::CodegenOptions opts;
    opts.checkedDisposal = false;
    auto r = runAotChecked(src, opts);
    return r.ran ? r.stdoutText : std::string(kDidNotRun);
}

std::string times(const std::string& line, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += line;
    return s;
}

// ---------------------------------------------------------------------------
// The stale-borrow shape (#7045's, with a dictionary literal): the holder
// stores a borrow of a literal temporary; the temporary is disposed at the end
// of the `do` block; the holder is used afterwards.
// ---------------------------------------------------------------------------

// `ContainsKey` through the stored borrow.
const char* kStaleContainsKey = R"BZL(
module Conf;
public class B
  private _s: Dictionary<int, int>;
  public constructor(s: Dictionary<int, int>)
    _s := s;
  end;
  public method Probe(): int
    if _s.ContainsKey(2) then return 1; end; return 0;
  end Probe;
end B;
public class Program
  public static method Main()
    var i := 0;
    while i < 100 do
      var h: B? := null;
      do
        h := new B([1: 10, 2: 20]);
      end;
      try
        Console.WriteLine(h!.Probe());
      catch
        DisposedAccessException => Console.WriteLine("faulted"),
      end try;
      i := i + 1;
    end;
  end Main;
end Program;
)BZL";

// `Add` through the stored borrow (a write into a released table).
const char* kStaleAdd = R"BZL(
module Conf;
public class B
  private _s: Dictionary<int, int>;
  public constructor(s: Dictionary<int, int>)
    _s := s;
  end;
  public method Probe(): int
    _s.Add(5, 50); return 7;
  end Probe;
end B;
public class Program
  public static method Main()
    var i := 0;
    while i < 100 do
      var h: B? := null;
      do
        h := new B([1: 10, 2: 20]);
      end;
      try
        Console.WriteLine(h!.Probe());
      catch
        DisposedAccessException => Console.WriteLine("faulted"),
      end try;
      i := i + 1;
    end;
  end Main;
end Program;
)BZL";

// `Remove` through the stored borrow.
const char* kStaleRemove = R"BZL(
module Conf;
public class B
  private _s: Dictionary<int, int>;
  public constructor(s: Dictionary<int, int>)
    _s := s;
  end;
  public method Probe(): int
    if _s.Remove(1) then return 1; end; return 0;
  end Probe;
end B;
public class Program
  public static method Main()
    var i := 0;
    while i < 100 do
      var h: B? := null;
      do
        h := new B([1: 10, 2: 20]);
      end;
      try
        Console.WriteLine(h!.Probe());
      catch
        DisposedAccessException => Console.WriteLine("faulted"),
      end try;
      i := i + 1;
    end;
  end Main;
end Program;
)BZL";

// a string-keyed dictionary (the other key representation).
const char* kStaleStringKeyed = R"BZL(
module Conf;
public class B
  private _s: Dictionary<string, int>;
  public constructor(s: Dictionary<string, int>)
    _s := s;
  end;
  public method Probe(): int
    if _s.ContainsKey("a") then return 1; end; return 0;
  end Probe;
end B;
public class Program
  public static method Main()
    var i := 0;
    while i < 100 do
      var h: B? := null;
      do
        h := new B(["a": 1, "b": 2]);
      end;
      try
        Console.WriteLine(h!.Probe());
      catch
        DisposedAccessException => Console.WriteLine("faulted"),
      end try;
      i := i + 1;
    end;
  end Main;
end Program;
)BZL";

// a `.Count` read of a dictionary through the stored borrow.
const char* kStaleDictionaryCount = R"BZL(
module Conf;
public class B
  private _s: Dictionary<int, int>;
  public constructor(s: Dictionary<int, int>)
    _s := s;
  end;
  public method Probe(): int
    return _s.Count;
  end Probe;
end B;
public class Program
  public static method Main()
    var i := 0;
    while i < 100 do
      var h: B? := null;
      do
        h := new B([1: 10, 2: 20]);
      end;
      try
        Console.WriteLine(h!.Probe());
      catch
        DisposedAccessException => Console.WriteLine("faulted"),
      end try;
      i := i + 1;
    end;
  end Main;
end Program;
)BZL";

// a `.Count` read of a dict-backed set through the stored borrow (#7076's own
// repro shape). Before: unguarded, so it read the released table.
const char* kStaleSetCount = R"BZL(
module Conf;
public class B
  private _s: Set<int>;
  public constructor(s: Set<int>)
    _s := s;
  end;
  public method Probe(): int
    return _s.Count;
  end Probe;
end B;
public class Program
  public static method Main()
    var i := 0;
    while i < 100 do
      var h: B? := null;
      do
        h := new B([1, 2, 3]);
      end;
      try
        Console.WriteLine(h!.Probe());
      catch
        DisposedAccessException => Console.WriteLine("faulted"),
      end try;
      i := i + 1;
    end;
  end Main;
end Program;
)BZL";

// ---------------------------------------------------------------------------
// The two boundary controls' programs.
// ---------------------------------------------------------------------------

// An owning dictionary's scope-end release cascades into each value exactly
// once (§28.11): 2 values per iteration, 100 iterations.
const char* kOwningDictionaryCascade = R"BZL(
module Conf;
public class Res
  public static Disposed: int := 0;
  public constructor()
  end;
  destructor
    Res.Disposed := Res.Disposed + 1;
  end destructor;
end Res;
public class Program
  public static method Main()
    var live := 0;
    var i := 0;
    while i < 100 do
      using var d := new OwningDictionary<int, Res>();
      d.Add(1, new Res());
      d.Add(2, new Res());
      live := live + d.Count;
      i := i + 1;
    end;
    Console.WriteLine(live);
    Console.WriteLine(Res.Disposed);
  end Main;
end Program;
)BZL";

// Live dictionaries and a set, released at scope end 100 times, none used
// after its release: a literal temporary passed as an argument, a literal
// bound to a local, a set literal, an `owned` result, and a `new` — 2 + 3 +
// 3 + 1 + 1 per iteration. The three literal-built tables are the ones whose
// retained header only the scope-end escape mark keeps reachable.
const char* kLiveDictionaries = R"BZL(
module Conf;
public class Program
  static method CountOf(d: Dictionary<int, int>): int
    return d.Count;
  end CountOf;
  static method Make(n: int): owned Dictionary<int, int>
    var r := new Dictionary<int, int>();
    r.Add(n, n);
    return r;
  end Make;
  public static method Main()
    var acc := 0;
    var i := 0;
    while i < 100 do
      acc := acc + CountOf([1: 10, 2: 20]);
      var lit: Dictionary<int, int> := [1: 10, 2: 20, 3: 30];
      acc := acc + lit.Count;
      var s: Set<int> := [1, 2, 3];
      acc := acc + s.Count;
      using var m := Make(i);
      acc := acc + m.Count;
      using var e := new Dictionary<string, int>();
      e.Add("k", i);
      if e.ContainsKey("k") then acc := acc + 1; end;
      i := i + 1;
    end;
    Console.WriteLine(acc);
  end Main;
end Program;
)BZL";

}  // namespace


// §28.2 / §28.3, v0.357: `ContainsKey` through the stored borrow.
BEZEL_TEST(codegen_checked_dict_scope_end_quarantine_7076, stale_borrow_containskey_faults_100x_28_2) {
    BEZEL_EXPECT_EQ(outChecked(kStaleContainsKey), times("faulted\n", 100));
}

// §28.2 / §28.3, v0.357: `Add` through the stored borrow (a write into a released table).
BEZEL_TEST(codegen_checked_dict_scope_end_quarantine_7076, stale_borrow_add_faults_100x_28_2) {
    BEZEL_EXPECT_EQ(outChecked(kStaleAdd), times("faulted\n", 100));
}

// §28.2 / §28.3, v0.357: `Remove` through the stored borrow.
BEZEL_TEST(codegen_checked_dict_scope_end_quarantine_7076, stale_borrow_remove_faults_100x_28_2) {
    BEZEL_EXPECT_EQ(outChecked(kStaleRemove), times("faulted\n", 100));
}

// §28.2 / §28.3, v0.357: a string-keyed dictionary (the other key representation).
BEZEL_TEST(codegen_checked_dict_scope_end_quarantine_7076, stale_borrow_string_keyed_containskey_faults_100x_28_2) {
    BEZEL_EXPECT_EQ(outChecked(kStaleStringKeyed), times("faulted\n", 100));
}

// §28.2 / §28.3, v0.357: a `.Count` read of a disposed dictionary.
BEZEL_TEST(codegen_checked_dict_scope_end_quarantine_7076, stale_borrow_dictionary_count_faults_100x_28_2) {
    BEZEL_EXPECT_EQ(outChecked(kStaleDictionaryCount), times("faulted\n", 100));
}

// §28.2 / §28.3, v0.357: a `.Count` read of a disposed dict-backed set — the
// issue's repro. It must fault, not answer from a released (or reset) table.
BEZEL_TEST(codegen_checked_dict_scope_end_quarantine_7076, stale_borrow_set_count_faults_100x_28_2) {
    BEZEL_EXPECT_EQ(outChecked(kStaleSetCount), times("faulted\n", 100));
}

// §28.11 / §28.2, v0.357. Boundary control 1: the quarantining release runs
// the same value cascade as the freeing one — every value disposed, none
// twice — in checked mode, in checked mode under ASan + LSan, and in
// unchecked mode under ASan + LSan. WRONG FIX it catches: a quarantine that
// keeps the header by skipping the release (or by only setting the flag) —
// the owned values are then never disposed and the destructor count is 0.
BEZEL_TEST(codegen_checked_dict_scope_end_quarantine_7076, control_owning_dictionary_cascade_runs_once_28_11) {
    BEZEL_EXPECT_EQ(outChecked(kOwningDictionaryCascade), "200\n200\n");
    BEZEL_EXPECT_EQ(outCheckedSan(kOwningDictionaryCascade), "200\n200\n");
    BEZEL_EXPECT_EQ(outUncheckedSan(kOwningDictionaryCascade), "200\n200\n");
}

// §28.2, v0.357: "The two modes give identical observable behavior for every
// program that never uses a disposed instance." Boundary control 2. The
// checked + LSan leg proves the 500 quarantined headers are retained
// reachably, not leaked; the unchecked + LSan leg proves the immediate free
// is still there. WRONG FIXES it catches: the scope-end quarantine without
// its escape mark (LeakSanitizer reports the 300 literal-built headers in the
// checked + ASan leg), and a quarantine not gated on checked mode
// (LeakSanitizer reports all 500 in the unchecked + ASan leg).
BEZEL_TEST(codegen_checked_dict_scope_end_quarantine_7076, control_live_dictionaries_scope_end_100x_28_2) {
    BEZEL_EXPECT_EQ(outChecked(kLiveDictionaries), "1000\n");
    BEZEL_EXPECT_EQ(outCheckedSan(kLiveDictionaries), "1000\n");
    BEZEL_EXPECT_EQ(outUncheckedSan(kLiveDictionaries), "1000\n");
}
