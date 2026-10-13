// Regression tests for #5997 — a hash container (Set<K>, Dictionary<K, _>)
// keyed by a record or choice that DECLARES its own equality pair compared
// and hashed the key by STRUCTURAL equality, never calling the user's
// `Equals` / `GetHashCode`. The declared members answer "equal" for any two
// keys (hash 0), so a Set of two keys must hold ONE element; the structural
// path held two.
//
// Spec: §3.6 (v0.338), hashable row: a record/value-like choice that declares
// its own equality (`Equals(other: C)` with `GetHashCode`, with or without a
// homogeneous `=`/`<>` beside them) is hashable on the strength of those
// members, so a hash container compares and hashes with them. §8.3 (equality
// table) and §4.8 (Dictionary key) agree.
//
// Rows assert the program's stdout (the observable), not the exit code.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// `Key` declares Equals(other: Key) => true and GetHashCode() => 0, with the
// homogeneous operators false-leaning in the second family below. Every
// instance is "equal" to every other, so a hash container must hold one.
const char* kRecordKey =
    "module M; "
    "record Key(X: int) "
    "  public method Equals(other: Key): bool => true; "
    "  public method GetHashCode(): int => 0; "
    "  public static method operator =(a: Key, b: Key): bool => true; "
    "  public static method operator <>(a: Key, b: Key): bool => false; "
    "end Key; ";

// Declared Equals is FALSE for every pair (a key is never equal to itself),
// so a hash container must hold every Add, even of the same structural value.
// The homogeneous operators are present because sema requires them beside the
// pair today (the pair-only shape is #5934, not this fix).
const char* kRecordKeyNeverEqual =
    "module M; "
    "record Key(X: int) "
    "  public method Equals(other: Key): bool => false; "
    "  public method GetHashCode(): int => 0; "
    "  public static method operator =(a: Key, b: Key): bool => false; "
    "  public static method operator <>(a: Key, b: Key): bool => true; "
    "end Key; ";

const char* kChoiceKey =
    "module M; "
    "choice S A(x: int), B; "
    "  public method Equals(o: S): bool => true; "
    "  public method GetHashCode(): int => 0; "
    "  public static method operator =(a: S, b: S): bool => true; "
    "  public static method operator <>(a: S, b: S): bool => false; "
    "end S; ";

// A record WITHOUT user equality still counts structurally (no regression).
const char* kPlainRecordKey = "module M; record Key(X: int); ";

std::string mainProg(const char* decls, const char* body) {
    return std::string(decls) +
           "class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

}  // namespace

// #5997 / §3.6 (v0.338): Set<Key> with a declared Equals => true, GetHashCode
// => 0 holds ONE element for two keys (the issue's run2.bzl repro, Count 1).
BEZEL_TEST(hash_user_equality_5997, set_record_user_equals_counts_one) {
    auto r = runAot(mainProg(kRecordKey,
        "  var s := new Set<Key>(); "
        "  s.Add(new Key(1)); s.Add(new Key(2)); "
        "  Console.WriteLine(s.Count.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// #5997 / §3.6: the same through a Dictionary key (chsn/dict repro): the second
// `Add`/index-store of an "equal" key overwrites the first, so Count is 1.
BEZEL_TEST(hash_user_equality_5997, dictionary_record_user_equals_counts_one) {
    auto r = runAot(mainProg(kRecordKey,
        "  var d := new Dictionary<Key, int>(); "
        "  d.Add(new Key(1), 1); d[new Key(2)] := 2; "
        "  Console.WriteLine(d.Count.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// #5997 / §3.6: a choice that declares the pair is hashed/compared by it.
BEZEL_TEST(hash_user_equality_5997, set_choice_user_equals_counts_one) {
    auto r = runAot(mainProg(kChoiceKey,
        "  var s := new Set<S>(); "
        "  s.Add(S.A(1)); s.Add(S.A(2)); "
        "  Console.WriteLine(s.Count.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// #5997 / §3.6: the declared Equals answers FALSE for two structurally equal
// keys, so the Set must hold both (Count 2). The structural path said 1.
BEZEL_TEST(hash_user_equality_5997, set_record_user_equals_false_counts_two) {
    auto r = runAot(mainProg(kRecordKeyNeverEqual,
        "  var s := new Set<Key>(); "
        "  s.Add(new Key(1)); s.Add(new Key(1)); "
        "  Console.WriteLine(s.Count.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// #5997 control: a record with NO user equality keeps structural equality, so
// two different keys are two elements and two equal keys are one.
BEZEL_TEST(hash_user_equality_5997, set_plain_record_stays_structural) {
    auto r = runAot(mainProg(kPlainRecordKey,
        "  var s := new Set<Key>(); "
        "  s.Add(new Key(1)); s.Add(new Key(2)); s.Add(new Key(1)); "
        "  Console.WriteLine(s.Count.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}
