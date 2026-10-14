// Codegen — issue #6270 / spec §28.5, §28.11 (v0.338): an `owned Set<T>` class
// field is disposed with its owner.
//
// THE DEFECT. A borrowing `Set<T>` whose element is generic-dict-eligible
// (records, non-nullable primitives / enums, strings, plain inline arrays) is
// constructed as a `bz_dict_t*` hash table, but the class's synthesized
// `DisposeCore` freed every non-dictionary, non-linked-list owned collection
// field with `bz_array_free`. On the hash table that freed the header and
// leaked the insertion-order array and every key copy — 3400 B per 50 owner
// instances at `T = int` under LeakSanitizer (the IR carried
// `call void @bz_array_free(ptr %2, ptr null)` in `IBag$DisposeCore`). A local
// `using var s := new Set<int>()` was clean: only the field path was wrong.
// §28.5: an owned field is disposed when its owner is; §28.11: disposing a
// collection releases what it holds.
//
// Every row loops 50 times under `runAotChecked` (always ASan + LSan), so a
// per-instance leak is a reported leak, not a one-shot below the noise.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

std::string times50(const std::string& once) {
    std::string s;
    for (int i = 0; i < 50; ++i) s += once;
    return s;
}

} // namespace

// Row 1 — THE ISSUE'S OWN SHAPE: a non-generic class with an `owned Set<int>`
// field, and the same field inside a generic `Bag<T>` at `T = int`. Pre-fix:
// LeakSanitizer reports the order array and key copies of every instance.
BEZEL_TEST(class_field_owned_set_dispose_6270,
           owned_set_int_field_is_freed_with_its_owner_spec_28_5) {
    auto r = runAotChecked(
        "module M; "
        "class IBag "
        "  private owned _s: Set<int> := new Set<int>(); "
        "  public method Add(x: int) _s.Add(x); end Add; "
        "  public method N(): int => _s.Count; "
        "end IBag; "
        "class Bag<T> where T: hashable "
        "  private owned _s: Set<T> := new Set<T>(); "
        "  public method Add(x: T) _s.Add(x); end Add; "
        "  public method N(): int => _s.Count; "
        "end Bag; "
        "class C public static method Main(): int "
        "  for var i in 0..49 do "
        "    using var b := new IBag(); b.Add(1); b.Add(1); b.Add(i); "
        "    using var g := new Bag<int>(); g.Add(7); g.Add(7); "
        "    Console.WriteLine(b.N() + g.N()); "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // i = 1 is the one iteration where Add(i) duplicates Add(1).
    std::string want;
    for (int i = 0; i < 50; ++i) want += (i == 1 ? "2\n" : "3\n");
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

// Row 2 — the other generic-backed element kinds through the same field
// path: a record element, a nullable-record element under a generic `Bag<T>`
// (`T = P?`, array-backed — the control that must keep its free), an enum
// element, and two Set fields in one class.
BEZEL_TEST(class_field_owned_set_dispose_6270,
           record_enum_and_multiple_set_fields_spec_28_11) {
    auto r = runAotChecked(
        "module M; "
        "enum E A, B; end E; "
        "record P(X: int, Y: int); "
        "class Bag<T> where T: hashable "
        "  private owned _s: Set<T> := new Set<T>(); "
        "  public method Add(x: T) _s.Add(x); end Add; "
        "  public method N(): int => _s.Count; "
        "end Bag; "
        "class Two "
        "  private owned _a: Set<P> := new Set<P>(); "
        "  private owned _e: Set<E> := new Set<E>(); "
        "  public method Fill() "
        "    _a.Add(new P(1, 2)); _a.Add(new P(1, 2)); _a.Add(new P(2, 1)); "
        "    _e.Add(E.A); _e.Add(E.A); "
        "  end Fill; "
        "  public method N(): int => _a.Count * 10 + _e.Count; "
        "end Two; "
        "class C public static method Main(): int "
        "  for var i in 0..49 do "
        "    using var p := new Bag<P>(); p.Add(new P(1, 2)); p.Add(new P(1, 2)); "
        "    using var q := new Bag<P?>(); q.Add(new P(1, 2)); q.Add(null); "
        "    using var t := new Two(); t.Fill(); "
        "    Console.WriteLine($\"{p.N()} {q.N()} {t.N()}\"); "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, times50("1 2 21\n"));
}
