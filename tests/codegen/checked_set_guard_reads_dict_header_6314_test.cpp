// Spec-derived regression test for issue #6314 (narrow: the Set / SortedSet
// group — nine of the issue's fourteen corpus cases).
//
// §28.2 (use after disposal; checked disposal mode), v0.357. stdlib §4.10
// (`Set<T>`), stdlib §4.18 (`SortedSet<T>`), §3.6 (a record / choice element's
// declared `Equals` / `GetHashCode`).
//
// THE RULE. §28.2, verbatim: "The two modes give identical observable behavior
// for every program that never uses a disposed instance." The corpus cases and
// the 100x rows below never use a disposed instance, so checked disposal mode
// must print exactly what unchecked mode prints.
//
// THE DEFECT. `tryLowerCollectionInstanceCall` emits its checked-mode
// method-entry guard as `bz_array_is_disposed(recv)` for every collection
// receiver. A `Set<T>` / `SortedSet<T>` whose element type has a generic-key
// backing is a `bz_dict_t`, not a `bz_array_t`: the byte the array guard reads
// (`bz_array_t::disposed`, offset 40) is the low byte of the dictionary's
// `eq_fn` pointer. So the FIRST method call on a LIVE set raised
// `DisposedAccessException` whenever that function's address did not end in a
// zero byte — decided by link layout, which is why two of the nine cases
// passed under AddressSanitizer and failed without it. The guard now reads the
// dictionary header's own flag (`bz_dict_is_disposed`).
//
// THE ORACLE. Each live-set row runs one program in checked disposal mode, in checked
// disposal mode under generated-code ASan + LSan, and in unchecked mode, and
// asserts the SAME stdout from all three. The nine corpus cases are embedded
// verbatim with the output their `//! output:` / `//! stdout:` header states; the remaining
// rows loop each set member 100 times, because one call on one set proves
// nothing about a guard whose verdict depended on an address.
//
// THE OTHER HALF. Once the guard reads the right flag it needs a header whose
// flag gets set. A set released at scope end keeps its header in checked mode
// (narrow #7076, the preceding commit); this commit makes an explicit
// `Dispose()` on a set quarantine too, as a dictionary's already does. So a
// member call on a disposed set — through the same local, or through a borrow
// stored before the disposal — raises `DisposedAccessException`, and a second
// `Dispose()` stays the no-op §28.2 says it is.
//
// OUT OF SCOPE (tracked, #7076): a `for ... in` over a disposed set is not
// guarded by the dictionary flag.

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

// §28.2 checked disposal mode under generated-code AddressSanitizer +
// LeakSanitizer. The link layout differs from the plain build, so this leg
// also covers a second set of function addresses.
std::string outCheckedSan(const std::string& src) {
    bezel::codegen::CodegenOptions opts;
    opts.checkedDisposal = true;
    auto r = runAotChecked(src, opts);
    return r.ran ? r.stdoutText : std::string(kDidNotRun);
}

// Unchecked mode — the other half of §28.2's "identical observable behavior".
std::string outUnchecked(const std::string& src) {
    bezel::codegen::CodegenOptions opts;
    opts.checkedDisposal = false;
    auto r = runAot(src, opts);
    return r.ran ? r.stdoutText : std::string(kDidNotRun);
}

// The three modes must agree, and agree on `want`.
#define EXPECT_ALL_MODES(src, want)                     \
    do {                                                \
        BEZEL_EXPECT_EQ(outChecked(src), want);         \
        BEZEL_EXPECT_EQ(outCheckedSan(src), want);      \
        BEZEL_EXPECT_EQ(outUnchecked(src), want);       \
    } while (0)

// ---------------------------------------------------------------------------
// The issue's own repros: the nine Set / SortedSet corpus cases, verbatim.
// ---------------------------------------------------------------------------

// tests/conformance/rules/cases/s3_6_0046.bzl (§3.6): a Set of records with a declared Equals and GetHashCode holds one element for two keys.
const char* kS3_6_0046 = R"BZL(
module Conf;
record Key(X: int)
  public method Equals(other: Key): bool => true;
  public method GetHashCode(): int => 0;
  public static method operator =(a: Key, b: Key): bool => true;
  public static method operator <>(a: Key, b: Key): bool => false;
end Key;
class C public static method Main(): int
  var s := new Set<Key>(); s.Add(new Key(1)); s.Add(new Key(2)); Console.WriteLine(s.Count.ToString());
  return 0;
end Main; end C;
)BZL";

// tests/conformance/rules/cases/s3_6_0048.bzl (§3.6): a Set of choices with a declared Equals and GetHashCode holds one element for two values.
const char* kS3_6_0048 = R"BZL(
module Conf;
choice S A(x: int), B;
  public method Equals(o: S): bool => true;
  public method GetHashCode(): int => 0;
  public static method operator =(a: S, b: S): bool => true;
  public static method operator <>(a: S, b: S): bool => false;
end S;
class C public static method Main(): int
  var s := new Set<S>(); s.Add(S.A(1)); s.Add(S.A(2)); Console.WriteLine(s.Count.ToString());
  return 0;
end Main; end C;
)BZL";

// tests/conformance/rules/cases/s3_6_0049.bzl (§3.6): a declared Equals that is false for every pair keeps both elements in a Set.
const char* kS3_6_0049 = R"BZL(
module Conf;
record Key(X: int)
  public method Equals(other: Key): bool => false;
  public method GetHashCode(): int => 0;
  public static method operator =(a: Key, b: Key): bool => false;
  public static method operator <>(a: Key, b: Key): bool => true;
end Key;
class C public static method Main(): int
  var s := new Set<Key>(); s.Add(new Key(1)); s.Add(new Key(1)); Console.WriteLine(s.Count.ToString());
  return 0;
end Main; end C;
)BZL";

// tests/conformance/rules/cases/s3_6_0050.bzl (§3.6): a record without user equality keeps structural equality in a Set.
const char* kS3_6_0050 = R"BZL(
module Conf;
record Key(X: int);
class C public static method Main(): int
  var s := new Set<Key>(); s.Add(new Key(1)); s.Add(new Key(2)); s.Add(new Key(1)); Console.WriteLine(s.Count.ToString());
  return 0;
end Main; end C;
)BZL";

// tests/conformance/rules/cases/l4_10_0001.bzl (stdlib §4.10): Set<T>.TryAdd reports false on a duplicate element.
const char* kL4_10_0001 = R"BZL(
module Conf;

public class Program
  public static method Main()
    using var s := new Set<int>();
    _ := s.TryAdd(1);
    Console.WriteLine(s.TryAdd(1));
  end Main;
end Program;
)BZL";

// tests/conformance/rules/cases/l4_10_0036.bzl (stdlib §4.10): Set<T>.Remove's result is a bool, usable in a condition.
const char* kL4_10_0036 = R"BZL(
module Conf;

public class Program
  public static method Main()
    var s := new Set<int>();
    s.Add(1);
    var b: bool := s.Remove(1);
    if b then Console.WriteLine("ok"); else Console.WriteLine("ok"); end if;
  end Main;
end Program;
)BZL";

// tests/conformance/rules/cases/l4_18_0001.bzl (stdlib §4.18): SortedSet<T> iterates in ascending order regardless of insertion order.
const char* kL4_18_0001 = R"BZL(
module Conf;

public class Program
  public static method Main()
    using var ss := new SortedSet<int>();
    ss.Add(3);
    ss.Add(1);
    ss.Add(2);
    for var x in ss do
      Console.WriteLine(x);
    end;
  end Main;
end Program;
)BZL";

// tests/conformance/rules/cases/l4_18_0002.bzl (stdlib §4.18): Add on an already-present element is a silent no-op.
const char* kL4_18_0002 = R"BZL(
module Conf;

public class Program
  public static method Main()
    using var ss := new SortedSet<int>();
    ss.Add(1);
    ss.Add(1);
    Console.WriteLine(ss.Count);
  end Main;
end Program;
)BZL";

// tests/conformance/rules/cases/l4_18_0003.bzl (stdlib §4.18): TryAdd on an already-present element returns false.
const char* kL4_18_0003 = R"BZL(
module Conf;

public class Program
  public static method Main()
    using var ss := new SortedSet<int>();
    ss.Add(1);
    Console.WriteLine(ss.TryAdd(1));
  end Main;
end Program;
)BZL";

// ---------------------------------------------------------------------------
// 100x loops: every set member that goes through the guarded dispatch.
// ---------------------------------------------------------------------------

// A sync `Main` that runs `body` 100 times, accumulating into `acc`, and
// prints `acc`.
std::string loop100(const std::string& body, const std::string& prelude = "") {
    return "module M; " + prelude +
           "class C public static method Main(): int "
           "  var acc := 0; "
           "  var i := 0; "
           "  while i < 100 do " +
           body +
           "    i := i + 1; "
           "  end; "
           "  Console.WriteLine(acc); "
           "  return 0; "
           "end Main; end C;";
}

// A set held in an owned field and reached through a method, and a set passed
// as a plain (borrow) parameter: the receiver is not a local slot.
const char* kSetFieldAndParam = R"BZL(
module Conf;

public class Bag
  private owned _items: Set<int>;

  public constructor()
    _items := new Set<int>();
  end constructor;

  public method Put(x: int): bool => _items.TryAdd(x);
  public method Has(x: int): bool => _items.Contains(x);
  public method Size(): int => _items.Count;
end Bag;

public class Program
  static method Fill(s: Set<int>, k: int)
    s.Add(k);
    s.Add(k + 1);
  end Fill;
  public static method Main()
    var total := 0;
    var i := 0;
    while i < 100 do
      using var b := new Bag();
      if b.Put(i) then total := total + 1; end;
      if b.Put(i) then total := total + 1000; end;
      if b.Has(i) then total := total + 1; end;
      using var s := new Set<int>();
      Fill(s, i);
      if s.Contains(i + 1) then total := total + b.Size() + s.Count; end;
      i := i + 1;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
)BZL";

// ---------------------------------------------------------------------------
// The stale-borrow shape (§28.3's store-rule residual, #7045's shape with a
// set literal): the holder stores a borrow of a literal temporary, the
// temporary is disposed at the end of the `do` block, and the holder calls a
// set member afterwards. The retargeted guard must still fault here.
// ---------------------------------------------------------------------------
const char* kStaleSetContains = R"BZL(
module Conf;
public class B
  private _s: Set<int>;
  public constructor(s: Set<int>)
    _s := s;
  end;
  public method Probe(): int
    if _s.Contains(2) then return 1; end; return 0;
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

std::string afterDispose(const std::string& use) {
    return "module M; class C public static method Main(): int "
           "  var i := 0; "
           "  while i < 100 do "
           "    try "
           "      using var x: Set<int> := [1, 2]; x.Dispose(); " + use +
           "      Console.WriteLine(\"no-raise\"); "
           "    catch "
           "      DisposedAccessException => Console.WriteLine(\"DAE\"), "
           "    end try; "
           "    i := i + 1; "
           "  end; "
           "  return 0; end Main; end C;";
}

// A borrow of a live local set stored in a holder; the local is then disposed
// EXPLICITLY and the holder used.
const char* kAliasThenExplicitDispose = R"BZL(
module Conf;
public class B
  private _s: Set<int>;
  public constructor(s: Set<int>)
    _s := s;
  end;
  public method Probe(): int
    if _s.Contains(2) then return 1; end; return 0;
  end Probe;
end B;
public class Program
  public static method Main()
    var i := 0;
    while i < 100 do
      using var x: Set<int> := [1, 2, 3];
      var h := new B(x);
      x.Dispose();
      try
        Console.WriteLine(h.Probe());
      catch
        DisposedAccessException => Console.WriteLine("faulted"),
      end try;
      i := i + 1;
    end;
  end Main;
end Program;
)BZL";

std::string times(const std::string& line, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += line;
    return s;
}

}  // namespace


BEZEL_TEST(codegen_checked_set_guard_reads_dict_header_6314, corpus_s3_6_0046_set_of_records_with_declared_equality_3_6) {
    EXPECT_ALL_MODES(std::string(kS3_6_0046), "1\n");
}

BEZEL_TEST(codegen_checked_set_guard_reads_dict_header_6314, corpus_s3_6_0048_set_of_choices_with_declared_equality_3_6) {
    EXPECT_ALL_MODES(std::string(kS3_6_0048), "1\n");
}

BEZEL_TEST(codegen_checked_set_guard_reads_dict_header_6314, corpus_s3_6_0049_set_with_always_false_equals_keeps_both_3_6) {
    EXPECT_ALL_MODES(std::string(kS3_6_0049), "2\n");
}

BEZEL_TEST(codegen_checked_set_guard_reads_dict_header_6314, corpus_s3_6_0050_set_of_records_without_user_equality_3_6) {
    EXPECT_ALL_MODES(std::string(kS3_6_0050), "2\n");
}

BEZEL_TEST(codegen_checked_set_guard_reads_dict_header_6314, corpus_l4_10_0001_set_tryadd_reports_false_on_duplicate_stdlib_4_10) {
    EXPECT_ALL_MODES(std::string(kL4_10_0001), "False\n");
}

BEZEL_TEST(codegen_checked_set_guard_reads_dict_header_6314, corpus_l4_10_0036_set_remove_result_is_a_bool_stdlib_4_10) {
    EXPECT_ALL_MODES(std::string(kL4_10_0036), "ok\n");
}

BEZEL_TEST(codegen_checked_set_guard_reads_dict_header_6314, corpus_l4_18_0001_sorted_set_iterates_ascending_stdlib_4_18) {
    EXPECT_ALL_MODES(std::string(kL4_18_0001), "1\n2\n3\n");
}

BEZEL_TEST(codegen_checked_set_guard_reads_dict_header_6314, corpus_l4_18_0002_sorted_set_duplicate_add_is_a_silent_no_op_stdlib_4_18) {
    EXPECT_ALL_MODES(std::string(kL4_18_0002), "1\n");
}

BEZEL_TEST(codegen_checked_set_guard_reads_dict_header_6314, corpus_l4_18_0003_sorted_set_tryadd_duplicate_returns_false_stdlib_4_18) {
    EXPECT_ALL_MODES(std::string(kL4_18_0003), "False\n");
}

// --- 100x loops ------------------------------------------------------------

// stdlib §4.10: Add / Contains / Remove / Clear on a fresh `Set<int>` per
// iteration. Per iteration: 2 distinct elements (+2), Contains hit (+1),
// Remove hit (+1), count after Clear (+0) = 4.
BEZEL_TEST(codegen_checked_set_guard_reads_dict_header_6314, set_int_add_contains_remove_clear_100x_stdlib_4_10) {
    EXPECT_ALL_MODES(loop100(
        "using var s := new Set<int>(); s.Add(i); s.Add(i); s.Add(i + 1); "
        "acc := acc + s.Count; "
        "if s.Contains(i) then acc := acc + 1; end; "
        "if s.Remove(i) then acc := acc + 1; end; "
        "s.Clear(); acc := acc + s.Count; "), "400\n");
}

// stdlib §4.10: TryAdd's result on a first and on a duplicate element.
BEZEL_TEST(codegen_checked_set_guard_reads_dict_header_6314, set_int_tryadd_100x_stdlib_4_10) {
    EXPECT_ALL_MODES(loop100(
        "using var s := new Set<int>(); "
        "if s.TryAdd(i) then acc := acc + 1; end; "
        "if s.TryAdd(i) then acc := acc + 1000; end; "), "100\n");
}

// stdlib §4.10: a set LITERAL receiver (the other allocation site).
BEZEL_TEST(codegen_checked_set_guard_reads_dict_header_6314, set_literal_contains_100x_stdlib_4_10) {
    EXPECT_ALL_MODES(loop100(
        "using var s: Set<int> := [1, 2, 3]; "
        "if s.Contains(2) then acc := acc + 1; end; "
        "if s.Contains(9) then acc := acc + 1000; end; "), "100\n");
}

// stdlib §4.10: a string element (a different hash / equality pair, so a
// different `eq_fn` address behind the byte the old guard read).
BEZEL_TEST(codegen_checked_set_guard_reads_dict_header_6314, set_string_add_contains_100x_stdlib_4_10) {
    EXPECT_ALL_MODES(loop100(
        "using var s := new Set<string>(); s.Add(\"a\"); s.Add(\"b\"); s.Add(\"a\"); "
        "acc := acc + s.Count; "
        "if s.Contains(\"b\") then acc := acc + 1; end; "), "300\n");
}

// stdlib §4.10: the set-algebra members take a second set.
BEZEL_TEST(codegen_checked_set_guard_reads_dict_header_6314, set_union_intersect_except_100x_stdlib_4_10) {
    EXPECT_ALL_MODES(loop100(
        "using var a: Set<int> := [1, 2, 3]; using var b: Set<int> := [3, 4]; "
        "using var u: Set<int> := [1, 2, 3]; u.UnionWith(b); acc := acc + u.Count; "
        "using var n: Set<int> := [1, 2, 3]; n.IntersectWith(b); acc := acc + n.Count; "
        "a.ExceptWith(b); acc := acc + a.Count; "), "700\n");
}

// stdlib §4.18: SortedSet Add (with a duplicate), ascending iteration, Remove.
// Per iteration: 1 + 2 + 3 in order (123), then count after Remove (2).
BEZEL_TEST(codegen_checked_set_guard_reads_dict_header_6314, sorted_set_add_iterate_remove_100x_stdlib_4_18) {
    EXPECT_ALL_MODES(loop100(
        "using var ss := new SortedSet<int>(); ss.Add(3); ss.Add(1); ss.Add(2); ss.Add(1); "
        "var d := 0; for var x in ss do d := d * 10 + x; end; "
        "acc := acc + d; "
        "if ss.Remove(2) then acc := acc + ss.Count; end; "), "12500\n");
}

// A set reached through an owned field and through a borrow parameter. Per
// iteration: Put new (+1), Put duplicate (+0), Has (+1), then b.Size() (1) +
// s.Count (2) = 5.
BEZEL_TEST(codegen_checked_set_guard_reads_dict_header_6314, set_field_and_param_receivers_100x_stdlib_4_10) {
    EXPECT_ALL_MODES(std::string(kSetFieldAndParam), "500\n");
}

// --- the two boundary controls ------------------------------------------------
//
// These two rows hold on the parent commit as well — for the WRONG reason:
// before this commit every set member call raised, live or disposed, so they
// printed `faulted` / `DAE` without the guard ever reading a disposed flag.
// Each goes RED under a wrong fix of this defect that makes all nine corpus
// cases pass, which is why they are kept (checked-mode behaviour; a corpus
// case, run unchecked, cannot express either).

// §28.2 / §28.3, v0.357. Boundary control 1. WRONG FIX it catches: the guard
// retargeted to the dictionary flag while the scope-end release of a set
// still frees its header (this commit without narrow #7076) — the guard then
// reads a freed header and the member walks a freed table (measured:
// SIGSEGV). A set released at scope end must still fault through a stored
// borrow.
BEZEL_TEST(codegen_checked_set_guard_reads_dict_header_6314, control_stale_borrow_set_contains_faults_100x_28_2) {
    BEZEL_EXPECT_EQ(outChecked(kStaleSetContains), times("faulted\n", 100));
}

// --- an explicit Dispose() on a set ------------------------------------------

// §28.2, v0.357: "any instance member access on a logically-disposed instance
// ... raises `DisposedAccessException`". Boundary control 2. Before:
// `x.Dispose()` itself raised (the false positive), so these printed `DAE`
// without ever disposing. WRONG FIX it catches: the guard retargeted (or
// skipped for a set) while an explicit `Dispose()` on a set still frees the
// table and nulls the slot — the member call then runs on a null table and
// nothing raises (measured: `no-raise`).
BEZEL_TEST(codegen_checked_set_guard_reads_dict_header_6314, control_set_member_after_explicit_dispose_raises_100x_28_2) {
    BEZEL_EXPECT_EQ(outChecked(afterDispose("if x.Contains(1) then Console.WriteLine(1); end; ")),
                    times("DAE\n", 100));
    BEZEL_EXPECT_EQ(outChecked(afterDispose("x.Add(3); ")), times("DAE\n", 100));
    BEZEL_EXPECT_EQ(outChecked(afterDispose("Console.WriteLine(x.Count); ")),
                    times("DAE\n", 100));
}

// §28.2, v0.357: "A further `Dispose()` call remains a no-op, not an error, in
// both modes." All three modes, and LSan-clean: the quarantined header is
// retained reachably and released exactly once.
BEZEL_TEST(codegen_checked_set_guard_reads_dict_header_6314, set_double_dispose_is_a_no_op_100x_28_2) {
    EXPECT_ALL_MODES(loop100(
        "using var x := new Set<int>(); x.Add(i); x.Dispose(); x.Dispose(); "
        "acc := acc + 1; "), "100\n");
}

// §28.2, v0.357: the paired spelling on a string set — the scope-end release
// of the same slot runs its key-release step on an already-disposed header.
// (Checked and unchecked plain builds only: an explicit `Dispose()` on a
// `Set<string>` does not release its strings in either mode, a separate
// pre-existing leak.)
BEZEL_TEST(codegen_checked_set_guard_reads_dict_header_6314, string_set_explicit_dispose_then_scope_end_100x_28_2) {
    const std::string src = loop100(
        "using var x := new Set<string>(); x.Add(\"a\"); x.Add(\"b\"); "
        "acc := acc + x.Count; x.Dispose(); ");
    BEZEL_EXPECT_EQ(outChecked(src), "200\n");
    BEZEL_EXPECT_EQ(outUnchecked(src), "200\n");
}

// §28.2 / §28.3, v0.357: a borrow stored before the explicit disposal. Before:
// the `Dispose()` raised. With the guard fixed and no quarantine, this read a
// freed table (measured SIGSEGV).
BEZEL_TEST(codegen_checked_set_guard_reads_dict_header_6314, set_alias_after_explicit_dispose_faults_100x_28_2) {
    BEZEL_EXPECT_EQ(outChecked(kAliasThenExplicitDispose), times("faulted\n", 100));
}
