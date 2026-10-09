// fix #5597 / l4_9_0009 (§4.9 / §30.14, v0.330): OwningDictionary with a
// class- or interface-typed key is not sendable — its keys are held as a
// borrowing map's are (spec §28.11), so the borrowing condition applies to
// them as well. At a class- or interface-typed K the map carries borrows and
// is not sendable, whatever K's own sendability.
//
// Rows:
//   R1  OwningDictionary<Key, Widget> with class Key — as Channel element — rejected
//   R2  OwningDictionary<int, Widget> with int key — as Channel element — accepted
//   R3  OwningDictionary<string, Widget> with string key — as Channel element — accepted
//
// Authority: stdlib §4.9 / spec §30.14.

#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndTypecheckGated;
using bezel::test::SemaResult;

// R1 — OwningDictionary with class-typed key is not sendable.
BEZEL_TEST(sendability_dict_class_key_5597,
           owning_dictionary_class_key_rejected_spec_30_14) {
    auto r = parseAndTypecheckGated(
        "module M; "
        "import System.Threading; "
        "sealed class Key "
        "  public Id: int := 1; "
        "end Key; "
        "sealed class Widget "
        "  public Size: int := 2; "
        "end Widget; "
        "class C "
        "  public method Demo() "
        "    using var ch := new Channel<OwningDictionary<Key, Widget>>(); "
        "    using var tx := ch.NewSender(); "
        "    var d := new OwningDictionary<Key, Widget>(); "
        "    tx.Send(move d); "
        "  end Demo; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// R2 — OwningDictionary with int key is sendable (value type).
BEZEL_TEST(sendability_dict_class_key_5597,
           owning_dictionary_int_key_accepted_spec_30_14) {
    auto r = parseAndTypecheckGated(
        "module M; "
        "import System.Threading; "
        "sealed class Widget "
        "  public Size: int := 2; "
        "end Widget; "
        "class C "
        "  public method Demo() "
        "    using var ch := new Channel<OwningDictionary<int, Widget>>(); "
        "    using var tx := ch.NewSender(); "
        "    var d := new OwningDictionary<int, Widget>(); "
        "    tx.Send(move d); "
        "  end Demo; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// R3 — OwningDictionary with string key is sendable (runtime-managed type).
BEZEL_TEST(sendability_dict_class_key_5597,
           owning_dictionary_string_key_accepted_spec_30_14) {
    auto r = parseAndTypecheckGated(
        "module M; "
        "import System.Threading; "
        "sealed class Widget "
        "  public Size: int := 2; "
        "end Widget; "
        "class C "
        "  public method Demo() "
        "    using var ch := new Channel<OwningDictionary<string, Widget>>(); "
        "    using var tx := ch.NewSender(); "
        "    var d := new OwningDictionary<string, Widget>(); "
        "    tx.Send(move d); "
        "  end Demo; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
