// Spec-derived regression for issue #8533: a stdlib handle with NO dispatch
// header (`Shared<T>`, `Weak<T>`, `LinkedList` / `OwningLinkedList`, the
// hash-backed `Dictionary` / `Set` families, the three event sources, the two
// atomic cells, the serializers and deserializers) stored in an `IDisposable`
// slot was disposed through a vtable it did not have — SEGV /
// heap-buffer-overflow.
//
// THE RULE
// ========
// Stdlib §2.10 (v0.357): "Every class implicitly implements `IDisposable`".
// The stdlib's own classes are classes (§2.16: "`public class Shared<T>:
// IDisposable`"; §4.6-§4.10 declare the collections as classes), so each of
// them converts to `IDisposable` and may be held in a slot of that type.
//
// §28.13 (v0.357): "**Handles are ordinary owned class instances.** Every rule
// of this chapter applies to a handle unchanged: it is produced owned (by
// `new Shared<T>(...)` or `Clone()`), discharged by `using var`, an `owned`
// field, an `owned` parameter or return, `move`, or explicit `Dispose()` ...
// A handle may be stored in owning collections (`OwningArray<Shared<T>>`) like
// any class instance."  And: "Disposing a handle decrements the count;
// disposing the last live handle disposes the underlying value".
//
// The three slot kinds, and the sentence that disposes each:
//
//   * an `owned d: IDisposable` PARAMETER — §28.3 K3.14 (v0.357): "An owned
//     value that reaches its scope's end undischarged and not moved-from is
//     disposed there implicitly" (the sentence goes on to name the
//     "undischarged `owned` parameter");
//   * an `owned F: IDisposable?` FIELD — §28.4 step 2 (v0.357): the generated
//     `Dispose()` "Disposes all owned-bearing fields in reverse declaration
//     order. This covers fields marked `owned` (class- or interface-typed; the
//     field's value's `Dispose()` is called)"; §28.5: the modifier
//     "**Auto-disposes** the field when the containing object is disposed";
//   * an `OwningArray<IDisposable>` ELEMENT — §28.11 (v0.357): "class /
//     interface type | borrows | **owned**", and the collection's `Dispose()`
//     "**disposes** every live element".
//
// What "the value's `Dispose()`" does is the value's OWN type's rule, whatever
// the slot's static type: §28.11 again for a collection held in the slot (an
// owning collection disposes its elements; a borrowing one's slots "are never
// disposed"), §28.13 for a handle.
//
// THE DEFECT (measured on f7c94ed6f = r30-c1 + the #7987 fix, spec v0.357)
// ========================================================================
// All three slots discharge by the class protocol: load word 0 as a vtable,
// call slot 2, reclaim the block.  These handles had no word-0 vtable:
//
//   * a `Shared<T>` / `Weak<T>` handle WAS its control block, whose word 0 is
//     the shared value — the dispatch called through the VALUE's vtable on a
//     40-byte block (`heap-buffer-overflow`), or through garbage;
//   * a `bz_dict_t` opens with its bucket pointer, a `bz_llist_t` with its
//     head node — `SEGV` at pc 0 / at 0x10.
//
// ASan, 20 iterations, every one of the 3 slots: `Shared<Res>`, a `Clone()`,
// `Weak<Res>` — heap-buffer-overflow; `Dictionary<int,int>`,
// `Dictionary<string,string>`, `OwningDictionary<int,Res>`,
// `SortedDictionary<int,int>`, `Set<int>`, `Set<string>`, `SortedSet<int>`,
// `LinkedList<int>`, `LinkedList<string>`, `OwningLinkedList<Res>` — SEGV.
// (The parameter rows crash on r30-c1 itself; the field and element rows
// leaked there and crash since #7987 made those two slots dispose.)
//
// The audit of every other `new`-able stdlib class found the same shape, with
// the same SEGV at all three slots, in: `EventSource<T>`,
// `AsyncEventSource<T>`, `SharedEventSource<T>` (one struct opening with the
// handler array), `Atomic<T>`, `AtomicInteger<T>` (one cell opening with its
// refcount), `BinarySerializer`, `JsonSerializer` (one struct opening with
// the output buffer), the two deserializers, and the `Subscription` an event
// `Subscribe` returns (a node opening with its source pointer).
//
// THE FIX
// =======
// Each of them now opens with a runtime vtable whose slot 2 is its real
// disposal (owner ruling on #7554: the slot must reach the value's real
// disposal; declining to dispose is not a remedy):
//
//   * a handle is its own small cell — vtable word + control-block pointer —
//     one per `Shared<T>` / `Weak<T>` value (stdlib §2.16: `Clone()` is "a new
//     independent handle"); slot 2 gives that handle's count back;
//   * `bz_llist_t` / `bz_dict_t` gained a leading vtable word and a record,
//     written where the value is constructed (the one place its static type
//     is known), of what its typed free runs: the element / value disposer,
//     and a set's two key steps.  Slot 2 runs that record;
//   * the event source, the atomic cell, the serializer, the deserializer
//     and the subscription structs gained the same leading vtable word; slot
//     2 does what the typed free does short of freeing the block (releases
//     the handler closures; nothing; the buffers; unsubscribes).
//
// Every row below asserts the destructor count on stdout — so a remedy that
// stopped crashing by NOT disposing fails here too — and is ASan/LSan-checked
// by the `runAotChecked` oracle (no leak, no double free, no wild read).

#include <cstdio>
#include <string>
#include <string_view>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

constexpr int kIters = 50;  // a one-shot repro is a vacuous leak oracle

// The declarations every row shares; `body` is the loop body, which may use
// the loop counter `i`.
std::string prog(const std::string& body) {
    return
        "module M;\n"
        "import System.Collections.Generic;\n"
        "import System.Threading;\n"
        "import System.Serialization;\n"
        "public class Res\n"
        "  public constructor() end constructor;\n"
        "  public method Ping(): int\n"
        "    return 7;\n"
        "  end Ping;\n"
        "  destructor\n"
        "    Console.WriteLine(\"RD\");\n"
        "  end destructor;\n"
        "end Res;\n"
        "public class G\n"
        "  public static method Take(owned d: IDisposable)\n"
        "  end Take;\n"
        "end G;\n"
        "public class H\n"
        "  public owned F: IDisposable? := null;\n"
        "  public constructor() end constructor;\n"
        "end H;\n"
        "public class C\n"
        "  public static method Main(): int\n"
        "    var i := 0;\n"
        "    while i < " + std::to_string(kIters) + " do\n" +
        body +
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(\"end\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
}

// The three slot kinds.  `setup` declares an owned local `c`; each form hands
// it to the slot, which then owns it and disposes it.
std::string atParameter(const std::string& setup) {
    return prog(setup + "      G.Take(move c);\n");
}
std::string atField(const std::string& setup) {
    return prog(setup +
                "      var h := new H();\n"
                "      h.F := c;\n");
}
std::string atElement(const std::string& setup) {
    return prog(setup +
                "      var a := new OwningArray<IDisposable>();\n"
                "      a.Add(move c);\n");
}

// `perIter` repeated once per loop iteration, then the `end` marker.
std::string expected(const std::string& perIter) {
    std::string out;
    for (int i = 0; i < kIters; ++i) out += perIter;
    return out + "end\n";
}

// The row must RUN, must be ASan/LSan-clean, and must print exactly `out`.
// There is deliberately NO absorbing `if (skipped) return;` guard: a
// toolchain SKIP must surface as a FAILURE, never as a silent pass.
void expectCleanUnderAsan(const std::string& program, const std::string& out) {
    JitResult r = runAotChecked(program);
    // Name the CAUSE of a red row: `!r.ran` alone does not say whether the
    // program was rejected, died on a signal or tripped the sanitizer.
    if (!r.ran || r.exitCode != 0)
        for (const auto& d : r.diagnostics.diagnostics())
            std::fprintf(stderr, "    [8533 row] %.400s\n", d.message.c_str());
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

// ---- the values ----------------------------------------------------------

// §28.13: the only handle — disposing it disposes the value (one `RD`).
const char* const kShared =
    "      var c := new Shared<Res>(new Res());\n";
// §28.13: a `Clone()` beside a live first handle — the value is disposed
// once, by whichever handle goes last (one `RD` per iteration).
const char* const kSharedClone =
    "      using var s0 := new Shared<Res>(new Res());\n"
    "      var c := s0.Clone();\n";
// §28.13: a weak handle "does not keep the underlying value alive"; the
// value is disposed by the strong handle (one `RD`).
const char* const kWeak =
    "      using var s0 := new Shared<Res>(new Res());\n"
    "      var c := s0.Downgrade();\n";
// §28.11 / stdlib §4.8: a borrowing dictionary of values — nothing to run.
const char* const kDictIntInt =
    "      var c := new Dictionary<int, int>();\n"
    "      c.Add(1, 2);\n";
// §28.9 / stdlib §4.8: string keys and values are runtime-managed; the table
// releases its references (LSan is the oracle).
const char* const kDictStrStr =
    "      var c := new Dictionary<string, string>();\n"
    "      c.Add(\"k\" + i.ToString(), \"v\" + i.ToString());\n";
// §28.11 / stdlib §4.9: an owning dictionary disposes every live value.
const char* const kOwningDict =
    "      var c := new OwningDictionary<int, Res>();\n"
    "      c.Add(1, new Res());\n"
    "      c.Add(2, new Res());\n";
// stdlib §4.17: the sorted map shares the hash-backed table.
const char* const kSortedDict =
    "      var c := new SortedDictionary<int, int>();\n"
    "      c.Add(1, 2);\n";
// stdlib §4.10: a hash-backed set of values.
const char* const kSetInt =
    "      var c := new Set<int>();\n"
    "      c.Add(1);\n";
// §28.9 / stdlib §4.10: a set of strings co-owns its elements.
const char* const kSetStr =
    "      var c := new Set<string>();\n"
    "      c.Add(\"x\" + i.ToString());\n";
// stdlib §4.18: the sorted set shares the hash-backed table.
const char* const kSortedSet =
    "      var c := new SortedSet<int>();\n"
    "      c.Add(1);\n";
// §28.11 / stdlib §4.6: a borrowing linked list of values.
const char* const kListInt =
    "      var c := new LinkedList<int>();\n"
    "      c.AddLast(1);\n";
// §28.9 / stdlib §4.6: a linked list of strings co-owns its elements.
const char* const kListStr =
    "      var c := new LinkedList<string>();\n"
    "      c.AddLast(\"x\" + i.ToString());\n";
// §28.11 / stdlib §4.7: an owning linked list disposes every live element.
const char* const kOwningList =
    "      var c := new OwningLinkedList<Res>();\n"
    "      c.AddLast(new Res());\n"
    "      c.AddLast(new Res());\n";

// Stdlib §6.5 (v0.357): an event source's destructor "severs the subscriber
// list before reclamation" — the source owns one reference on each handler
// closure, so a disposal that skipped the list would leak the closure.
const char* const kEventSource =
    "      var c := new EventSource<int>();\n"
    "      c.Add(lambda(x) => do Console.WriteLine(x); end);\n"
    "      c.Raise(3);\n";
const char* const kAsyncEventSource =
    "      var c := new AsyncEventSource<int>();\n";
const char* const kSharedEventSource =
    "      var c := new SharedEventSource<int>();\n";
// Stdlib §6.4: the atomic cells.
const char* const kAtomic =
    "      var c := new Atomic<int>(1);\n"
    "      c.Value := 5;\n"
    "      Console.WriteLine(c.Value);\n";
const char* const kAtomicInteger =
    "      var c := new AtomicInteger<long>(1);\n"
    "      Console.WriteLine(c.Increment());\n";
// Stdlib §2.15: the concrete serializers (each owns its output buffer) and
// the JSON deserializer built from a string (it owns its UTF-8 copy).
const char* const kBinarySerializer =
    "      var c := new BinarySerializer();\n"
    "      c.WriteInt(5);\n";
const char* const kJsonSerializer =
    "      var c := new JsonSerializer();\n"
    "      c.WriteInt(5);\n";
const char* const kJsonDeserializer =
    "      var c := new JsonDeserializer(\"42\");\n"
    "      Console.WriteLine(c.ReadInt());\n";

}  // namespace

// ---- §28.3 K3.14: the `owned d: IDisposable` PARAMETER --------------------
// Each of these crashed on r30-c1 itself (the parameter path predates #7987).

BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_13_shared_handle_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kShared), expected("RD\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_13_shared_clone_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kSharedClone), expected("RD\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_13_weak_handle_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kWeak), expected("RD\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_11_dictionary_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kDictIntInt), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_9_string_dictionary_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kDictStrStr), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_11_owning_dictionary_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kOwningDict), expected("RD\nRD\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_11_sorted_dictionary_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kSortedDict), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_11_set_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kSetInt), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_9_string_set_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kSetStr), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_11_sorted_set_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kSortedSet), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_11_linked_list_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kListInt), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_9_string_linked_list_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kListStr), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_11_owning_linked_list_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kOwningList), expected("RD\nRD\n"));
}

// ---- §28.4 step 2 / §28.5: the `owned F: IDisposable?` FIELD --------------
// Each of these leaked on r30-c1 and crashed once #7987 disposed the field.

BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_13_shared_handle_at_owned_field) {
    expectCleanUnderAsan(atField(kShared), expected("RD\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_13_shared_clone_at_owned_field) {
    expectCleanUnderAsan(atField(kSharedClone), expected("RD\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_13_weak_handle_at_owned_field) {
    expectCleanUnderAsan(atField(kWeak), expected("RD\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_11_dictionary_at_owned_field) {
    expectCleanUnderAsan(atField(kDictIntInt), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_9_string_dictionary_at_owned_field) {
    expectCleanUnderAsan(atField(kDictStrStr), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_11_owning_dictionary_at_owned_field) {
    expectCleanUnderAsan(atField(kOwningDict), expected("RD\nRD\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_11_sorted_dictionary_at_owned_field) {
    expectCleanUnderAsan(atField(kSortedDict), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_11_set_at_owned_field) {
    expectCleanUnderAsan(atField(kSetInt), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_9_string_set_at_owned_field) {
    expectCleanUnderAsan(atField(kSetStr), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_11_sorted_set_at_owned_field) {
    expectCleanUnderAsan(atField(kSortedSet), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_11_linked_list_at_owned_field) {
    expectCleanUnderAsan(atField(kListInt), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_9_string_linked_list_at_owned_field) {
    expectCleanUnderAsan(atField(kListStr), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_11_owning_linked_list_at_owned_field) {
    expectCleanUnderAsan(atField(kOwningList), expected("RD\nRD\n"));
}

// ---- §28.11: the `OwningArray<IDisposable>` ELEMENT -----------------------
// Each of these leaked on r30-c1 and crashed once #7987 disposed the element.

BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_13_shared_handle_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kShared), expected("RD\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_13_shared_clone_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kSharedClone), expected("RD\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_13_weak_handle_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kWeak), expected("RD\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_11_dictionary_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kDictIntInt), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_9_string_dictionary_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kDictStrStr), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_11_owning_dictionary_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kOwningDict), expected("RD\nRD\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_11_sorted_dictionary_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kSortedDict), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_11_set_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kSetInt), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_9_string_set_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kSetStr), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_11_sorted_set_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kSortedSet), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_11_linked_list_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kListInt), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_9_string_linked_list_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kListStr), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_11_owning_linked_list_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kOwningList), expected("RD\nRD\n"));
}

// ---- the issue's own repros, verbatim (a fresh `new` at the slot) ---------

BEZEL_TEST(idisposable_slot_headerless_handle_8533, issue_repro_fresh_shared_at_all_three_slots) {
    expectCleanUnderAsan(
        prog("      G.Take(new Shared<Res>(new Res()));\n"
             "      var h := new H();\n"
             "      h.F := new Shared<Res>(new Res());\n"
             "      var a := new OwningArray<IDisposable>();\n"
             "      a.Add(new Shared<Res>(new Res()));\n"),
        expected("RD\nRD\nRD\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, issue_repro_fresh_owning_dictionary_at_all_three_slots) {
    expectCleanUnderAsan(
        prog("      G.Take(new OwningDictionary<int, Res>());\n"
             "      var h := new H();\n"
             "      h.F := new OwningDictionary<int, Res>();\n"
             "      var a := new OwningArray<IDisposable>();\n"
             "      a.Add(new OwningDictionary<int, Res>());\n"),
        expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, issue_repro_fresh_dictionary_and_owning_list_at_owned_field) {
    expectCleanUnderAsan(
        prog("      var h := new H();\n"
             "      h.F := new Dictionary<int, int>();\n"
             "      var h2 := new H();\n"
             "      h2.F := new OwningLinkedList<Res>();\n"),
        expected(""));
}

// ---- §28.13: what the erased disposal of a handle does to the VALUE -------

// "Disposing a handle decrements the count; disposing the last live handle
// disposes the underlying value" — so disposing ONE of two handles through
// the slot leaves the value alive for the other: `Get()` still reaches it
// (`7`) and its destructor runs only when `s0` goes, AFTER the read.
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_13_erased_dispose_of_one_handle_keeps_the_value_alive) {
    expectCleanUnderAsan(
        prog("      using var s0 := new Shared<Res>(new Res());\n"
             "      var c := s0.Clone();\n"
             "      G.Take(move c);\n"
             "      Console.WriteLine(s0.Get().Ping().ToString());\n"),
        expected("7\nRD\n"));
}

// The LAST handle disposed through the slot disposes the value THERE — before
// the caller's next statement — and a weak handle then observes it gone
// ("`TryUpgrade()` ... `null` otherwise").
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_13_erased_dispose_of_the_last_handle_disposes_the_value) {
    expectCleanUnderAsan(
        prog("      var s0 := new Shared<Res>(new Res());\n"
             "      using var w := s0.Downgrade();\n"
             "      G.Take(move s0);\n"
             "      Console.WriteLine(\"after\");\n"
             "      using var up := w.TryUpgrade();\n"
             "      if up = null then\n"
             "        Console.WriteLine(\"gone\");\n"
             "      end if;\n"),
        expected("RD\nafter\ngone\n"));
}

// A weak handle disposed through the slot gives back only its WEAK count: a
// second weak handle still upgrades while the strong handle lives.
BEZEL_TEST(idisposable_slot_headerless_handle_8533, spec_28_13_erased_dispose_of_a_weak_handle_leaves_the_value_upgradable) {
    expectCleanUnderAsan(
        prog("      using var s0 := new Shared<Res>(new Res());\n"
             "      var w := s0.Downgrade();\n"
             "      using var w2 := s0.Downgrade();\n"
             "      G.Take(move w);\n"
             "      using var up := w2.TryUpgrade();\n"
             "      if up <> null then\n"
             "        Console.WriteLine(\"up\");\n"
             "      end if;\n"),
        expected("up\nRD\n"));
}

// ---- the other header-less classes the audit found, at all three slots ----
// Event sources (stdlib §6.5), atomic cells (§6.4), serializers and the JSON
// deserializer (§2.15): classes, hence `IDisposable` (stdlib §2.10), disposed
// by the slot under the same three sentences as above (§28.3 K3.14 for the
// parameter, §28.4 step 2 / §28.5 for the field, §28.11 for the element).
// Each SEGV'd on f7c94ed6f.

BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_6_5_event_source_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kEventSource), expected("3\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_6_5_async_event_source_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kAsyncEventSource), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_6_5_shared_event_source_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kSharedEventSource), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_6_4_atomic_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kAtomic), expected("5\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_6_4_atomic_integer_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kAtomicInteger), expected("2\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_2_15_binary_serializer_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kBinarySerializer), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_2_15_json_serializer_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kJsonSerializer), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_2_15_json_deserializer_at_owned_parameter) {
    expectCleanUnderAsan(atParameter(kJsonDeserializer), expected("42\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_6_5_event_source_at_owned_field) {
    expectCleanUnderAsan(atField(kEventSource), expected("3\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_6_5_async_event_source_at_owned_field) {
    expectCleanUnderAsan(atField(kAsyncEventSource), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_6_5_shared_event_source_at_owned_field) {
    expectCleanUnderAsan(atField(kSharedEventSource), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_6_4_atomic_at_owned_field) {
    expectCleanUnderAsan(atField(kAtomic), expected("5\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_6_4_atomic_integer_at_owned_field) {
    expectCleanUnderAsan(atField(kAtomicInteger), expected("2\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_2_15_binary_serializer_at_owned_field) {
    expectCleanUnderAsan(atField(kBinarySerializer), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_2_15_json_serializer_at_owned_field) {
    expectCleanUnderAsan(atField(kJsonSerializer), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_2_15_json_deserializer_at_owned_field) {
    expectCleanUnderAsan(atField(kJsonDeserializer), expected("42\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_6_5_event_source_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kEventSource), expected("3\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_6_5_async_event_source_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kAsyncEventSource), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_6_5_shared_event_source_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kSharedEventSource), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_6_4_atomic_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kAtomic), expected("5\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_6_4_atomic_integer_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kAtomicInteger), expected("2\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_2_15_binary_serializer_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kBinarySerializer), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_2_15_json_serializer_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kJsonSerializer), expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_2_15_json_deserializer_at_owning_array_element) {
    expectCleanUnderAsan(atElement(kJsonDeserializer), expected("42\n"));
}

// ---- stdlib §2.11: a `Subscription` ---------------------------------------
// Its destructor "removes the registration" (stdlib §2.11, v0.357), which is
// observable: a handler whose subscription the slot has ALREADY disposed
// must not run at the next `Raise`, and one whose
// subscription the slot still holds must. So the parameter row prints
// nothing (the callee disposed it at its scope end, §28.3 K3.14), and the
// field / element rows print the payload once (the slot's owner is still
// alive at the `Raise`).  Each SEGV'd on f7c94ed6f.

BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_2_11_subscription_at_owned_parameter) {
    expectCleanUnderAsan(
        prog("      using var e := new EventSource<int>();\n"
             "      G.Take(e.Subscribe(lambda(x) => do Console.WriteLine(x); end));\n"
             "      e.Raise(1);\n"),
        expected(""));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_2_11_subscription_at_owned_field) {
    expectCleanUnderAsan(
        prog("      using var e := new EventSource<int>();\n"
             "      var h := new H();\n"
             "      h.F := e.Subscribe(lambda(x) => do Console.WriteLine(x); end);\n"
             "      e.Raise(1);\n"),
        expected("1\n"));
}
BEZEL_TEST(idisposable_slot_headerless_handle_8533, stdlib_2_11_subscription_at_owning_array_element) {
    expectCleanUnderAsan(
        prog("      using var e := new EventSource<int>();\n"
             "      var a := new OwningArray<IDisposable>();\n"
             "      a.Add(e.Subscribe(lambda(x) => do Console.WriteLine(x); end));\n"
             "      e.Raise(1);\n"),
        expected("1\n"));
}
