// fix #9196 (§28.3 K3.1 + *Captured owned bindings*, §28.11, v0.359): a store of
// a CAPTURED owned binding into an owning-collection slot, written inside a
// lambda body, is a compile error.
//
// Spec (docs/Bezel_Language_Spec.md, §28.3.7 move semantics, anchor
// sec-captured-owned-bindings, v0.359): "Inside a closure, a captured owned
// binding — an owned local or `owned` parameter of the enclosing body — is
// uniformly in borrow state: ... assignment to it, a marked transfer from it,
// an explicit `Dispose()` or discard of it, and every other owned-consuming use
// of it are compile errors — a closure cannot consume, reassign, or dispose a
// captured owned binding". §28.3's consume table row K3.1 makes "store into an
// `owned` field or owning-collection slot" a consuming position, and §28.11
// gives the owning family's indexer-set the owned column ("takes `owned`"), so
// `_items[key] := item;` over a captured owned `item` is an owned-consuming use.
//
// The base admitted it: the lambda-body walk never reaches `handleAssignment`'s
// K3.1 arm, so the store drew only a false §28.3 must-use warning, and a read of
// `item` afterwards (in the lambda, or in the method after the call) compiled
// and SEGFAULTED (the slot had moved the value out of the captured cell).
//
// A lambda's OWN `owned` parameter is not captured: the closure owns it, the
// store consumes it, and that program must keep compiling and running clean.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::checkedToolchainSkipped;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

template <class Diags>
bool hasCannotTransfer(const Diags& ds, const char* name) {
    const std::string needle = std::string("cannot transfer captured owned "
                                           "binding `") + name +
                               "` inside a closure";
    for (const auto& d : ds.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// The rejection oracle: a COMPILE-time failure at a real location carrying the
// specific "cannot transfer captured owned binding `name`" diagnostic.
void expectCapturedTransferRejected(const std::string& src, const char* name) {
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(hasCannotTransfer(r.diagnostics, name));
}

const char* kUser =
    "public class User(id: int) "
    "  public readonly property Id: int := id; "
    "  destructor Console.WriteLine($\"close {Id}\"); end destructor; "
    "end User; ";

const char* kMain =
    "public class Program "
    "  public static method Main() "
    "    using var r := new Repo(); "
    "    r.Go(1, new User(1)); "
    "    Console.WriteLine(\"done\"); "
    "  end Main; "
    "end Program; ";

std::string repo(const std::string& method) {
    return std::string("module M; ") + kUser +
           "public class Repo "
           "  private owned _items: OwningDictionary<int, User> := "
           "new OwningDictionary<int, User>(); " +
           method + "end Repo; " + kMain;
}

}  // namespace

// ROW 1 — the issue's repro: a lambda-body store of a captured `owned`
// PARAMETER. RED on the base (admitted, only a false warning).
BEZEL_TEST(lambda_body_owning_slot_store_of_captured_owned_9196,
           captured_owned_param_store_into_owning_slot_rejected) {
    expectCapturedTransferRejected(
        repo("public method Go(key: int, owned item: User) "
             "  var f := lambda(): int => do _items[key] := item; return 0; end; "
             "  _ := f(); "
             "end Go; "),
        "item");
}

// ROW 2 — the generic twin (`OwningDictionary<K, T>`), same store.
BEZEL_TEST(lambda_body_owning_slot_store_of_captured_owned_9196,
           generic_captured_owned_param_store_into_owning_slot_rejected) {
    expectCapturedTransferRejected(
        "module M; " + std::string(kUser) +
            "public class Repo<K, T> where K: hashable where T: class "
            "  private owned _items: OwningDictionary<K, T> := "
            "new OwningDictionary<K, T>(); "
            "  public method Go(key: K, owned item: T) "
            "    var f := lambda(): int => do _items[key] := item; return 0; end; "
            "    _ := f(); "
            "  end Go; "
            "end Repo; "
            "public class Program "
            "  public static method Main() "
            "    using var r := new Repo<int, User>(); "
            "    r.Go(1, new User(1)); "
            "    Console.WriteLine(\"done\"); "
            "  end Main; "
            "end Program; ",
        "item");
}

// ROW 3 — the reviewer's segfault shape (e): the lambda stores `item` and then
// READS it. Base: compiles, SEGV at run time (null deref).
BEZEL_TEST(lambda_body_owning_slot_store_of_captured_owned_9196,
           store_then_read_in_lambda_rejected) {
    expectCapturedTransferRejected(
        repo("public method Go(key: int, owned item: User) "
             "  var f := lambda(): int => do _items[key] := item; "
             "return item.Id; end; "
             "  _ := f(); "
             "end Go; "),
        "item");
}

// ROW 4 — the reviewer's segfault shape (g): the lambda stores `item`, the
// METHOD then reads it after the call. Base: compiles, SEGV at run time.
BEZEL_TEST(lambda_body_owning_slot_store_of_captured_owned_9196,
           store_in_lambda_then_read_in_method_rejected) {
    expectCapturedTransferRejected(
        repo("public method Go(key: int, owned item: User) "
             "  var f := lambda(): int => do _items[key] := item; return 0; end; "
             "  _ := f(); "
             "  Console.WriteLine(item.Id); "
             "end Go; "),
        "item");
}

// ROW 5 — the captured owned LOCAL half of the same rule (#9217's shape).
BEZEL_TEST(lambda_body_owning_slot_store_of_captured_owned_9196,
           captured_owned_local_store_into_owning_slot_rejected) {
    expectCapturedTransferRejected(
        repo("public method Go(key: int, owned item: User) "
             "  var u := new User(2); "
             "  var f := lambda(): int => do _items[key] := u; return 0; end; "
             "  _ := f(); "
             "  Console.WriteLine(\"after\"); "
             "end Go; "),
        "u");
}

// CONTROL (green on the base, named per the lane contract): the lambda's OWN
// `owned` parameter is not captured — the closure owns it and the store moves
// it into the slot. 200 iterations, counting destructor: exactly one disposal
// per object, no leak, no double free. Red under a wrong fix that rejects every
// lambda-body store of an owned parameter (the rejection arm over-reaching to
// the lambda's own parameter) and under one that disposes the value twice.
BEZEL_TEST(lambda_body_owning_slot_store_of_captured_owned_9196,
           lambda_own_owned_param_store_disposes_each_object_once) {
    const std::string src =
        "module M; "
        "public class User(id: int) "
        "  public static disposed: int := 0; "
        "  public readonly property Id: int := id; "
        "  destructor disposed := disposed + 1; end destructor; "
        "end User; "
        "public class Repo "
        "  private owned _items: OwningDictionary<int, User> := "
        "new OwningDictionary<int, User>(); "
        "  public method Go(key: int, owned item: User) "
        "    var f := lambda(owned u: User): int => do _items[key] := u; "
        "return 0; end; "
        "    _ := f(move item); "
        "  end Go; "
        "end Repo; "
        "public class Program "
        "  public static method Main() "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      using var r := new Repo(); "
        "      r.Go(i, new User(i)); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(User.disposed); "
        "  end Main; "
        "end Program; ";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n"));
}
