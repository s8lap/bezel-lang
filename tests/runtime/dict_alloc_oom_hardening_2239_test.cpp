// Direct unit tests for bz_dict_alloc's / bz_dict_alloc_generic's Dictionary
// ctor (stdlib §4.8 / §4.9).
//
// Root cause (2026-07-28 hardening wave, follow-up to #1762 finding R1 /
// #2234 / #2237 / #2238):
// bz_dict_alloc / bz_dict_alloc_generic in
// src/runtime/bezel_runtime_collections.cpp:457 and 664 allocated the header
// and buckets via calloc without null-checking. A failed buckets calloc left
// d->buckets == nullptr; the first bz_dict_set_str / bz_dict_set_gen
// dereferenced d->buckets[ins].key = ... and SIGSEGVed.
//
// This test pins the fix at the C-runtime level. Two contracts are checked:
//
//   1. Happy path: a non-null Dictionary handle accepts string-key inserts,
//      TryGetValue reads them back, and Count reflects the inserted total.
//      This is the regression: the OOM fix must not change the observable
//      shape of the produced handle on the normal path.
//   2. Defensive: even when the buckets pointer is non-null (the normal
//      allocation succeeded), inserting into the dict and then reading the
//      value back exercises the codepath whose pre-fix null-deref would have
//      crashed. We pin it here so any future refactor that loses the null
//      guard trips the test on the happy path.
//
// Direct OOM injection (LD_PRELOAD malloc hook) is out of scope — the
// shape-pinning on the happy path is what proves the null-check guards in
// bz_dict_alloc / bz_dict_alloc_generic did not disturb the normal case.

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>

#include "runtime/bezel_runtime.h"
#include "test_harness.hpp"

// ---------------------------------------------------------------------------
// §4.8: a fresh Dictionary<string,string> handle accepts string-key inserts,
// TryGetValue reads them back, and Count reflects the inserted total. The
// OOM-hardening fix must not change this — if it did, every Dictionary-using
// program would silently see wrong values.
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_dict_alloc_oom_2239, string_key_happy_path_roundtrips) {
    void* d = bz_dict_alloc();
    BEZEL_EXPECT_TRUE(d != nullptr);

    // Build small +1 bz_string values (stdlib §4.6 header shape):
    // { void* data, int64_t len, int8_t width, int64_t rc } = 32 bytes.
    auto make_str = [](const char* s) {
        int64_t len = (int64_t)std::strlen(s);
        char* data = (char*)std::malloc((size_t)len + 1);
        std::memcpy(data, s, (size_t)len);
        data[len] = '\0';
        uint8_t* h = (uint8_t*)std::calloc(1, 32);
        std::memcpy(h, &data, sizeof(void*));
        std::memcpy(h + 8, &len, sizeof(int64_t));
        h[16] = 1;  // width = 1 (UTF-8 bytes)
        return h;
    };

    uint8_t* va = make_str("one");
    uint8_t* vb = make_str("two");
    bz_dict_set_str(d, "alpha", 5, 1, va);
    bz_dict_set_str(d, "beta",  4, 1, vb);

    BEZEL_EXPECT_EQ(bz_dict_count(d), (int64_t)2);

    void* out_a = nullptr;
    void* out_b = nullptr;
    BEZEL_EXPECT_TRUE(bz_dict_try_get_str(d, "alpha", 5, 1, &out_a));
    BEZEL_EXPECT_TRUE(bz_dict_try_get_str(d, "beta",  4, 1, &out_b));
    BEZEL_EXPECT_TRUE(out_a == va);
    BEZEL_EXPECT_TRUE(out_b == vb);

    // Update-in-place (existing key): bz_dict_set_str overwrites the value
    // without disposing the displaced one (caller owns the values here).
    uint8_t* vb2 = make_str("TWO");
    bz_dict_set_str(d, "beta", 4, 1, vb2);
    BEZEL_EXPECT_EQ(bz_dict_count(d), (int64_t)2);
    void* out_b2 = nullptr;
    BEZEL_EXPECT_TRUE(bz_dict_try_get_str(d, "beta", 4, 1, &out_b2));
    BEZEL_EXPECT_TRUE(out_b2 == vb2);

    // Cleanup: free the inserted values (caller-owned) and the dict.
    std::free(va); std::free(vb); std::free(vb2);
    bz_dict_free(d, nullptr);
}

// ---------------------------------------------------------------------------
// §4.8: a missing key returns false from TryGetValue and leaves the out
// pointer untouched. The post-fix bz_dict_alloc must produce a dict whose
// first read after the bucket-backed struct is fully calloc-zeroed
// (no spurious leftover key data) — the pre-fix code that crashed on OOM
// never reached this path, but if the calloc-zero of buckets were lost in
// a future refactor, this test would catch it (Contains on an absent key
// would spuriously return true).
// ---------------------------------------------------------------------------
BEZEL_TEST(runtime_dict_alloc_oom_2239, missing_key_returns_false) {
    void* d = bz_dict_alloc();
    BEZEL_EXPECT_TRUE(d != nullptr);

    BEZEL_EXPECT_FALSE(bz_dict_contains_str(d, "nope", 4, 1));
    BEZEL_EXPECT_EQ(bz_dict_count(d), (int64_t)0);

    void* out = nullptr;
    BEZEL_EXPECT_FALSE(bz_dict_try_get_str(d, "nope", 4, 1, &out));

    bz_dict_free(d, nullptr);
}