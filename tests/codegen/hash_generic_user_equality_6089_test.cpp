// Regression tests for #6089 — a GENERIC record or choice that declares its
// own equality pair (`Equals(other: T)` + `GetHashCode(): int`, with the
// homogeneous operators) was still compared and hashed STRUCTURALLY by the
// hash containers (Set, Dictionary, the `where T: hashable` generic caller).
// The §5997 trampolines (buildRecordHashTrampoline and friends,
// LlvmBackend_Calls3.cpp) call the user's functions only for non-generic types:
// userValueEqualityPair declined every generic type, because a template method
// body is not a call target. The fix builds the pair per instantiation through
// buildSpecializedMethodFn, under the trampoline's ambient substitution.
//
// Spec: §3.6 (v0.338): a record or choice that declares the pair is hashable on
// the strength of those members; §8.3: equality and hash for hash containers
// follow them. Rows assert the program's stdout (the observable), not the exit
// code. Each user-semantics row differs from the structural answer, so a
// structural fallback fails it.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// `G<T>` declares Equals(other: G<T>) => true and GetHashCode() => 7: every
// instance is "equal" to every other, so a hash container must hold one.
const char* kGenericRecordEqual =
    "module M; "
    "record G<T>(X: T) "
    "  public method Equals(other: G<T>): bool => true; "
    "  public method GetHashCode(): int => 7; "
    "  public static method operator =(a: G<T>, b: G<T>): bool => true; "
    "  public static method operator <>(a: G<T>, b: G<T>): bool => false; "
    "end G; ";

// Declared Equals is FALSE for every pair, so a hash container holds every Add.
const char* kGenericRecordNeverEqual =
    "module M; "
    "record G<T>(X: T) "
    "  public method Equals(other: G<T>): bool => false; "
    "  public method GetHashCode(): int => 7; "
    "  public static method operator =(a: G<T>, b: G<T>): bool => false; "
    "  public static method operator <>(a: G<T>, b: G<T>): bool => true; "
    "end G; ";

const char* kGenericChoiceEqual =
    "module M; "
    "choice C<T> V(x: T), W; "
    "  public method Equals(o: C<T>): bool => true; "
    "  public method GetHashCode(): int => 3; "
    "  public static method operator =(a: C<T>, b: C<T>): bool => true; "
    "  public static method operator <>(a: C<T>, b: C<T>): bool => false; "
    "end C; ";

// A generic record with NO user equality keeps the structural form.
const char* kPlainGenericRecord = "module M; record H<T>(X: T); ";

std::string mainProg(const char* decls, const char* body) {
    return std::string(decls) +
           "class P public static method Main(): int " + body +
           " return 0; end Main; end P;";
}

}  // namespace

// #6089 / §3.6: Set<G<int>> with a declared Equals => true holds ONE element
// for two keys (the structural form holds two).
BEZEL_TEST(hash_generic_user_equality_6089, set_generic_record_user_equals_counts_one) {
    auto r = runAot(mainProg(kGenericRecordEqual,
        "  using var s := new Set<G<int>>(); "
        "  s.Add(new G<int>(1)); s.Add(new G<int>(2)); "
        "  Console.WriteLine(s.Count.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// #6089 / §3.6: the declared Equals answers FALSE for every pair, so two
// structurally equal keys are two elements (the structural form holds one).
BEZEL_TEST(hash_generic_user_equality_6089, set_generic_record_user_equals_false_counts_two) {
    auto r = runAot(mainProg(kGenericRecordNeverEqual,
        "  using var s := new Set<G<int>>(); "
        "  s.Add(new G<int>(1)); s.Add(new G<int>(1)); "
        "  Console.WriteLine(s.Count.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// #6089 / §3.6: a generic record key of a Dictionary, through TryAdd (the
// duplicate-tolerant insert): the second key is "equal", so TryAdd declines.
BEZEL_TEST(hash_generic_user_equality_6089, dictionary_generic_record_user_equals_counts_one) {
    auto r = runAot(mainProg(kGenericRecordEqual,
        "  var d := new Dictionary<G<int>, int>(); "
        "  d.TryAdd(new G<int>(1), 10); d.TryAdd(new G<int>(2), 20); "
        "  Console.WriteLine(d.Count.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// #6089 / §3.6: a generic CHOICE with the declared pair is hashed/compared by
// it (the structural form holds two distinct payload values).
BEZEL_TEST(hash_generic_user_equality_6089, set_generic_choice_user_equals_counts_one) {
    auto r = runAot(mainProg(kGenericChoiceEqual,
        "  using var s := new Set<C<int>>(); "
        "  s.Add(C<int>.V(1)); s.Add(C<int>.V(2)); "
        "  Console.WriteLine(s.Count.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// #6089 / §3.6: a `where T: hashable` generic caller instantiated at G<int>
// inserts through its own Set<T>; the instantiation's pair decides the count.
BEZEL_TEST(hash_generic_user_equality_6089, where_hashable_caller_at_generic_record_counts_one) {
    auto r = runAot(std::string(kGenericRecordEqual) +
        "class P "
        "public static method Cnt<T>(a: T, b: T): int where T: hashable "
        "  using var s := new Set<T>(); s.Add(a); s.Add(b); return s.Count; "
        "end Cnt; "
        "public static method Main(): int "
        "  Console.WriteLine(Cnt(new G<int>(1), new G<int>(2)).ToString()); "
        "  return 0; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// #6089 / §3.6: TWO instantiations of the same generic record in one program
// (G<int> and G<string>) each use the pair of their own instantiation: both
// hold one element (the structural form holds two in each).
BEZEL_TEST(hash_generic_user_equality_6089, two_instantiations_each_use_own_pair) {
    auto r = runAot(mainProg(kGenericRecordEqual,
        "  using var a := new Set<G<int>>(); "
        "  a.Add(new G<int>(1)); a.Add(new G<int>(2)); "
        "  using var b := new Set<G<string>>(); "
        "  b.Add(new G<string>(\"x\")); b.Add(new G<string>(\"y\")); "
        "  Console.WriteLine(a.Count.ToString() + \" \" + b.Count.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1 1\n"));
}

// #6089 control: a generic record with NO user equality stays structural, so
// two equal keys are one element and a different key is a second.
BEZEL_TEST(hash_generic_user_equality_6089, plain_generic_record_stays_structural) {
    auto r = runAot(mainProg(kPlainGenericRecord,
        "  using var s := new Set<H<int>>(); "
        "  s.Add(new H<int>(1)); s.Add(new H<int>(2)); s.Add(new H<int>(1)); "
        "  Console.WriteLine(s.Count.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// ---- Field-reading pairs (#6089 review). A pair whose Equals READS FIELDS is
// the realistic shape; the constant pairs above cannot see a wrong `other`.
// Each row's answer differs from the structural answer and from the answer the
// pre-fix trampoline gave (the `other` argument was passed as a pointer).

namespace {

// `G<T>` compares only its N field, so keys with the same N are one element.
const char* kFieldRecord =
    "module M; "
    "record G<T>(X: T, N: int) "
    "  public method Equals(other: G<T>): bool => N = other.N; "
    "  public method GetHashCode(): int => N; "
    "  public static method operator =(a: G<T>, b: G<T>): bool => a.N = b.N; "
    "  public static method operator <>(a: G<T>, b: G<T>): bool => a.N <> b.N; "
    "end G; ";

// A choice whose equality reads its payload field n (tag + n, through `case`).
const char* kFieldChoice =
    "module M; "
    "choice C<T> V(x: T, n: int), W; "
    "  public method Equals(o: C<T>): bool => "
    "    (case this of V(var x, var n) => (case o of V(var y, var m) => n = m, _ => false), "
    "     _ => (case o of W => true, _ => false)); "
    "  public method GetHashCode(): int => 3; "
    "  public static method operator =(a: C<T>, b: C<T>): bool => a.Equals(b); "
    "  public static method operator <>(a: C<T>, b: C<T>): bool => not a.Equals(b); "
    "end C; ";

}  // namespace

// #6089 / §3.6: Set<G<int>> keyed by N: (1,5), (2,5), (1,6) hold two elements
// (structural three; the pre-fix trampoline answered three).
BEZEL_TEST(hash_generic_user_equality_6089, set_field_reading_pair_counts_by_field) {
    auto r = runAot(mainProg(kFieldRecord,
        "  using var s := new Set<G<int>>(); "
        "  s.Add(new G<int>(1, 5)); s.Add(new G<int>(2, 5)); s.Add(new G<int>(1, 6)); "
        "  Console.WriteLine(s.Count.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// #6089 / §3.6: the same over a G<string> key (heap-owned X): four keys with
// two distinct N values hold two elements (structural four).
BEZEL_TEST(hash_generic_user_equality_6089, set_field_reading_pair_string_key) {
    auto r = runAot(mainProg(kFieldRecord,
        "  using var s := new Set<G<string>>(); "
        "  s.Add(new G<string>(\"a\", 1)); s.Add(new G<string>(\"b\", 1)); "
        "  s.Add(new G<string>(\"c\", 2)); s.Add(new G<string>(\"d\", 2)); "
        "  Console.WriteLine(s.Count.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// #6089 / §3.6: Dictionary<G<int>, int> with a field-reading key: TryAdd of a
// key equal by N declines, and the lookup by an equal key finds 10 (structural:
// count 3 and a missing key).
BEZEL_TEST(hash_generic_user_equality_6089, dictionary_field_reading_pair_lookup) {
    auto r = runAot(mainProg(kFieldRecord,
        "  var d := new Dictionary<G<int>, int>(); "
        "  d.Add(new G<int>(1, 5), 10); d.TryAdd(new G<int>(2, 5), 20); "
        "  d.Add(new G<int>(3, 6), 5); "
        "  Console.WriteLine(d.Count.ToString()); "
        "  Console.WriteLine(d[new G<int>(9, 5)].ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n10\n"));
}

// #6089 / §35.7: a generic CHOICE with a payload field read by its Equals:
// (1,5), (2,5), (3,6) hold two elements (structural three).
BEZEL_TEST(hash_generic_user_equality_6089, set_generic_choice_field_reading_pair) {
    auto r = runAot(mainProg(kFieldChoice,
        "  using var s := new Set<C<int>>(); "
        "  s.Add(C<int>.V(1, 5)); s.Add(C<int>.V(2, 5)); s.Add(C<int>.V(3, 6)); "
        "  Console.WriteLine(s.Count.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// #6089 / §3.6: a NESTED generic key G<G<int>> with a field-reading pair: the
// outer N decides, so two distinct N values hold two elements (structural three).
BEZEL_TEST(hash_generic_user_equality_6089, set_nested_generic_field_reading_pair) {
    auto r = runAot(mainProg(kFieldRecord,
        "  using var s := new Set<G<G<int>>>(); "
        "  s.Add(new G<G<int>>(new G<int>(1, 0), 5)); "
        "  s.Add(new G<G<int>>(new G<int>(2, 0), 5)); "
        "  s.Add(new G<G<int>>(new G<int>(3, 0), 6)); "
        "  Console.WriteLine(s.Count.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// #6089 / §3.6: G<int> and G<string> in one program, each with its own
// instantiation's field-reading pair: 2 and 3 (structural 3 and 5).
BEZEL_TEST(hash_generic_user_equality_6089, two_instantiations_field_reading_pairs) {
    auto r = runAot(mainProg(kFieldRecord,
        "  using var a := new Set<G<int>>(); "
        "  a.Add(new G<int>(1, 5)); a.Add(new G<int>(2, 5)); a.Add(new G<int>(3, 6)); "
        "  using var b := new Set<G<string>>(); "
        "  b.Add(new G<string>(\"a\", 1)); b.Add(new G<string>(\"b\", 1)); "
        "  b.Add(new G<string>(\"c\", 2)); b.Add(new G<string>(\"d\", 3)); "
        "  b.Add(new G<string>(\"e\", 3)); "
        "  Console.WriteLine(a.Count.ToString() + \" \" + b.Count.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2 3\n"));
}

// #6089 / §3.6 under ASan: 300 heap-owned string keys in 30 N-classes, then a
// Contains and a Remove by an equal key (Count shows the removal; the Remove
// result is not printed, see the note in the commit body). Structural holds
// 300 and removes none.
BEZEL_TEST(hash_generic_user_equality_6089, string_heap_keys_300_add_remove_contains) {
    auto r = runAot(mainProg(kFieldRecord,
        "  using var s := new Set<G<string>>(); "
        "  var i := 0; "
        "  while i < 300 do "
        "    s.Add(new G<string>(\"k\" + i.ToString(), i / 10)); i := i + 1; "
        "  end; "
        "  Console.WriteLine(s.Count.ToString()); "
        "  Console.WriteLine(s.Contains(new G<string>(\"q\", 7)).ToString()); "
        "  s.Remove(new G<string>(\"q\", 7)); "
        "  Console.WriteLine(s.Count.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("30\nTrue\n29\n"));
}
