// Regression test for #2058 — identity-hash side-table grows unbounded.
//
// Spec: Bezel_Language_Spec.md v0.204
//   §8.3 (sec-identity-hash-assignment): assigned identity hashes — the
//     class-default `GetHashCode` and the box-default identity hash draw from
//     a per-domain deterministic counter on first request and are stored in a
//     process-wide side-table keyed by object pointer.
//   §30.13 (sec-determinism): the deterministic counter sequence; the fix
//     preserves it (the reap removes only the freed object's entry; the counter
//     is NEVER reset/reused, so cross-run determinism is unchanged).
//
// Background: the side-table added by #2055 maps object pointer → assigned hash
// and was NEVER cleared, so for a long-running service that hashes many
// transient objects the table grew without bound. The #2058 fix reaps each
// entry at the object's free site (bz_obj_identity_hash_forget emitted by
// emitDisposeAndReclaim for owned classes, called by bz_box_release for boxes),
// bounding growth to the live hashed-object count.
//
// This test demonstrates bounded growth by allocating, hashing, and freeing
// many transient class instances inside a loop and asserting the side-table
// size afterwards stays BOUNDED (a small constant), not ≈ the iteration count.
// It uses the test-only debug accessor bz_identity_hash_table_size() exposed to
// Bezel via an `extern "c"` declaration (§32 FFI). Without the fix, the table
// accumulates one entry per iteration (size == iteration count ≫ threshold),
// so the assertion fails; with the fix, freed objects' entries are reaped and
// the table tracks only the few live hashed objects.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

// The test program. `iterations` is the number of alloc+hash+free cycles; the
// assertion is that the post-loop side-table size is BOUNDED well below that.
static std::string sidetableSizeAfterCycles(int iterations) {
    // `extern "c" bz_identity_hash_table_size` resolves to the runtime's
    // test/debug accessor (§32 FFI, by-name C linkage). The runtime is linked
    // into every AOT binary, so the symbol resolves at link time.
    std::string src =
        "module M; "
        "extern \"c\" public method bz_identity_hash_table_size(): int; end; "
        "class Plain public _v: int := 0; end Plain; "
        "class C public static method Main(): int "
        "  using var live := new Plain(); "          // stays live through the run
        "  var sink := live.GetHashCode(); "         // one entry that persists
        "  var i := 0; "
        "  while i < " + std::to_string(iterations) + " do "
        "    using var tmp := new Plain(); "         // fresh heap block / iteration
        "    var h := tmp.GetHashCode(); "           // hashed → side-table entry
        "    sink := sink + h; "                     // consume h (thwarts DCE)
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(bz_identity_hash_table_size()); "
        "  return sink; "
        "end Main; end C;";
    return runAotOut(src);
}

// §8.3 / #2058: the side-table is BOUNDED — after N=2000 alloc+hash+free
// cycles the table size is a small constant (the live hashed-object count),
// NOT ≈ N. Without the reap, the size would be ~N+1 and exceed the threshold
// (the test would fail). The threshold leaves headroom for any runtime-
// incidental hashed objects (e.g. a boxed value hashed transiently by an
// Console.WriteLine internal); the property under test is "bounded << N", not
// an exact count.
BEZEL_TEST(identity_hash_sidetable_reap_2058, table_bounded_after_many_cycles) {
    const int N = 2000;
    const int THRESHOLD = 50;   // << N; without the reap this would be ~N+1
    std::string out = sidetableSizeAfterCycles(N);
    BEZEL_EXPECT_FALSE(out.empty());
    BEZEL_EXPECT_FALSE(out == std::string("<did-not-run>"));
    int size = std::atoi(out.c_str());
    BEZEL_EXPECT_TRUE(size >= 0);                    // sanity: accessor returned
    BEZEL_EXPECT_TRUE(size < THRESHOLD);             // the bound — #2058 contract
}

// §8.3 / #2058: the same bound holds for BOXED value types — a boxed record
// reached through Object uses the box-default identity-hash thunk, so its
// side-table entry must also be reaped when the box is released. Demonstrates
// the bz_box_release reap path (not just the owned-class codegen path).
BEZEL_TEST(identity_hash_sidetable_reap_2058, table_bounded_after_boxed_cycles) {
    const int N = 2000;
    const int THRESHOLD = 50;
    std::string src =
        "module M; "
        "extern \"c\" public method bz_identity_hash_table_size(): int; end; "
        "record Rec public x: int; public constructor(x: int) this.x := x; "
        "  end constructor; end Rec; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  var sink := 0; "
        "  while i < " + std::to_string(N) + " do "
        "    var r := new Rec(i); "
        "    var o: Object := r; "                   // boxes Rec → box-default hash
        "    sink := sink + o.GetHashCode(); "       // hashed → side-table entry
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(bz_identity_hash_table_size()); "
        "  return sink; "
        "end Main; end C;";
    std::string out = runAotOut(src);
    BEZEL_EXPECT_FALSE(out.empty());
    BEZEL_EXPECT_FALSE(out == std::string("<did-not-run>"));
    int size = std::atoi(out.c_str());
    BEZEL_EXPECT_TRUE(size >= 0);
    BEZEL_EXPECT_TRUE(size < THRESHOLD);             // boxes reaped too
}
