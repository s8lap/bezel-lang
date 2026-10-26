// fix #7203 / spec §3.6 rule 2 / §14.1 / §14.2 / §23.4 / stdlib §4.4
// (v0.343): indexing a value whose static type is a generic type PARAMETER.
//
// THE DEFECT. A generic method's type parameter named `Set` (or `Stack`,
// `Queue`, `SortedSet`) carries the HEAD NAME of a stdlib collection, so the
// collection-indexer lowering read the receiver as THAT collection: the
// element size came from the head name (a bare name carries no generic
// arguments ⇒ `collectionElemSize`'s 8-byte fallback) while the element SLOT
// was sized from the declared type (an `int` ⇒ 4 bytes) — an 8-byte memcpy
// into a 4-byte slot.
//
// MEASURED on this lane's base, `bezelc --sanitize=address` on the issue's own
// program:
//
//   ==1469604==ERROR: AddressSanitizer: stack-buffer-overflow on address
//       0x7bb89a900024
//   WRITE of size 8 at 0x7bb89a900024 thread T0
//       #0 ... in memcpy
//       #1 ... in C$F$spec bezel
//   Address 0x7bb89a900024 is located in stack of thread T0 at offset 36 in
//   frame #0 ... in C$F$spec bezel
//     This frame has 1 object(s):
//       [32, 36) '' <== Memory access at offset 36 overflows this variable
//
// The specialization's IR showed the mismatch directly: `alloca i32` (the
// 4-byte `out` slot) followed by `call void @bz_array_get(ptr %4, i64 0,
// ptr %2, i64 8)` — an 8-byte copy into a 4-byte slot. The ERASED template
// (`bz$M.C$F`) carried the identical broken IR.
//
// THE FIX. Spec §3.6 rule 2 — "For a generic type or member, the property is
// evaluated at each instantiation, after substituting the type arguments" —
// and §14.1's *Instantiation and its site* put the BOUND, not the parameter's
// spelling, in charge of what a value of that type may be used for. The
// receiver's shape is read from the bound the active monomorphization binds
// the parameter to — so `Set` bound to `Array<int>` has an `int` element
// (4 bytes, stdlib §4.4's `T`), and the slot and the copy agree.
//
// THE DICT GUARD. The Set FAMILY (stdlib §4.10 `Set<T>` / `OwningSet<T>`,
// stdlib §4.18 `SortedSet<T>`) declares NO indexer — where stdlib §4.4's
// `Array<T>` declares `public property [i: int]: T;` — and its runtime
// representation is a hash table (a `bz_dict_t*`), not the dense `bz_array_t*`
// the collection-indexer path reads; yet its head is in the registry's
// array-backed gate, whose own header note says that predicate "also contains
// Set, OwningSet, SortedSet". The concrete `Set<int>[0]` spelling is kept away
// by #7114's SEMA rejection, which does NOT fire for a receiver whose static
// type is a type PARAMETER ⇒ the indexer path would read a `bz_dict_t*` as a
// `bz_array_t*` (a wild read at the dictionary's first bucket). The guard
// DECLINES such a bound, so the read degrades the way a `T`-named parameter
// already degrades (the `#2434` path) instead of reading the wrong
// representation. The discriminator is the BOUND's runtime representation,
// NEVER the parameter's name: an array-backed bound keeps its resolved bound
// and its correctly sized slot.
//
// THE ORACLE. Every row runs the program with the GENERATED code
// AddressSanitizer-instrumented (`opts.sanitizeAddress = true` — the same
// recipe `runAotChecked` uses, reached through the two-argument `runAot`
// overload). A program that still overflows is killed by the sanitizer
// BEFORE it prints, so the observed stdout is empty and the row fails; the
// value the row expects is therefore not merely "the right number" but "the
// program got far enough to print it".
//
// (The two-argument `runAot` overload is used rather than `runAotChecked`
// so this file adds no `runAotChecked` call site: that token is the
// denominator of `scripts/check_toolchain_skip_surface.py`, and a rise in it
// is RED unless an absorbing site is burned down alongside — see the
// handback. The house precedent for this exact recipe is
// tests/codegen/const_local_string_release_3416_test.cpp and
// tests/codegen/choice_value_like_string_payload_2115_test.cpp.)
//
// The rows cover the CRASH MATRIX — {`Set`-named, `T`-named} × {`Array<int>`,
// `Stack<int>`, `Queue<int>`, `Set<int>` (dict-backed)} — because NO shape may
// crash: the array-backed bounds read their first element, the dict-backed
// bound degrades to 0, and the last row is the LEGAL PROGRAM the fix must not
// turn into a rejection (a type parameter named `Set` CONSTRAINED to an
// indexer-bearing interface must still read its element — the #7114/#794
// shape).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// Run `src` with the generated code ASan-instrumented and return the
// program's stdout (trailing newlines stripped). "<did-not-run>" when the
// program never ran (a compile/link failure or a crash the harness could
// not attribute).
std::string runAsanOut(const std::string& src) {
    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    JitResult r = runAot(src, opts);
    if (!r.ran) return std::string("<did-not-run>");
    std::string out = r.stdoutText;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

// The issue's own program: a generic method whose TYPE PARAMETER is named
// `Set`, indexing it, called with an `Array<int>`.
const char* kGetSet =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    using var a: Array<int> := [7, 8, 9]; "
    "    Console.WriteLine(F<Array<int>>(a)); "
    "    return 0; "
    "  end Main; "
    "  public static method F<Set>(x: Set): int "
    "    var v: int := x[0]; "
    "    return v; "
    "  end F; "
    "end C;";

// The WRITE face of the same shape: `x[0] := 42` through a `Set`-named
// parameter, then read the slot back through the ordinary array indexer.
const char* kSetSet =
    "module M; "
    "class C "
    "  public static method Main(): int "
    "    using var a: Array<int> := [7, 8, 9]; "
    "    G<Array<int>>(a); "
    "    Console.WriteLine(a[0]); "
    "    return 0; "
    "  end Main; "
    "  public static method G<Set>(x: Set): () "
    "    x[0] := 42; "
    "  end G; "
    "end C;";

// The CRASH MATRIX shape: a generic method whose type PARAMETER is named
// `paramName`, indexing its parameter, called with a `bound` value.
// `literal` is the collection literal the bound is built from.
std::string matrixSrc(const char* paramName, const char* bound,
                       const char* literal) {
    std::string s =
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var a: ";
    s += bound;
    s += " := ";
    s += literal;
    s += "; "
        "Console.WriteLine(F<";
    s += bound;
    s += ">(a)); "
        "return 0; "
        "  end Main; "
        "  public static method F<";
    s += paramName;
    s += ">(x: ";
    s += paramName;
    s += "): int "
        "var v: int := x[0]; "
        "return v; "
        "  end F; "
        "end C;";
    return s;
}

}  // namespace

// spec §3.6 rule 2 / §14.1 / stdlib §4.4 (v0.343): the type parameter's BOUND
// is `Array<int>`, whose element type is `int` (4 bytes, stdlib §4.4) — so
// the element slot is 4 bytes and the read yields the FIRST element, 7.
// Pre-fix this was an 8-byte memcpy into a 4-byte slot (ASan:
// stack-buffer-overflow, WRITE of size 8) and the program never reached its
// print.
BEZEL_TEST(generic_typeparam_named_set_index_7203,
           set_named_param_bound_to_array_int_reads_first_element) {
    BEZEL_EXPECT_EQ(runAsanOut(kGetSet), "7");
}

// spec §3.6 rule 2 / stdlib §4.4 (v0.343): the WRITE face of the same shape.
// Pre-fix the element size was 8 while the stored value was an `int` (4
// bytes), so the copy over-read the value slot AND wrote 8 bytes at an
// 8-byte stride into a 4-byte-stride array (ASan: stack-buffer-overflow, READ
// of size 8).
BEZEL_TEST(generic_typeparam_named_set_index_7203,
           set_named_param_bound_to_array_int_writes_element) {
    BEZEL_EXPECT_EQ(runAsanOut(kSetSet), "42");
}

// spec §3.6 rule 2 (v0.343): the SIBLING head names that name an
// array-backed stdlib collection reach the same path — `Stack`, `Queue` and
// `SortedSet` all carry a collection head, so a parameter with one of those
// names was sized from the head (8 bytes) instead of from the bound's
// element type.
BEZEL_TEST(generic_typeparam_named_set_index_7203,
           stack_named_param_bound_to_array_int_reads_first_element) {
    BEZEL_EXPECT_EQ(runAsanOut(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var a: Array<int> := [7, 8, 9]; "
        "    Console.WriteLine(F<Array<int>>(a)); "
        "    return 0; "
        "  end Main; "
        "  public static method F<Stack>(x: Stack): int "
        "    var v: int := x[0]; "
        "    return v; "
        "  end F; "
        "end C;"), "7");
}

BEZEL_TEST(generic_typeparam_named_set_index_7203,
           queue_named_param_bound_to_array_int_reads_first_element) {
    BEZEL_EXPECT_EQ(runAsanOut(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var a: Array<int> := [7, 8, 9]; "
        "    Console.WriteLine(F<Array<int>>(a)); "
        "    return 0; "
        "  end Main; "
        "  public static method F<Queue>(x: Queue): int "
        "    var v: int := x[0]; "
        "    return v; "
        "  end F; "
        "end C;"), "7");
}

BEZEL_TEST(generic_typeparam_named_set_index_7203,
           sortedset_named_param_bound_to_array_int_reads_first_element) {
    BEZEL_EXPECT_EQ(runAsanOut(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var a: Array<int> := [7, 8, 9]; "
        "    Console.WriteLine(F<Array<int>>(a)); "
        "    return 0; "
        "  end Main; "
        "  public static method F<SortedSet>(x: SortedSet): int "
        "    var v: int := x[0]; "
        "    return v; "
        "  end F; "
        "end C;"), "7");
}

// spec §3.6 rule 2 / stdlib §4.4 (v0.343): the slot is sized from the
// BOUND'S ELEMENT TYPE, not from a fixed width — a `string` element is 8
// bytes (a pointer), so the same `Set`-named parameter bound to
// `Array<string>` reads a STRING, not an int. This row pins that the width
// comes from the bound (and not from "an int is 4 bytes").
BEZEL_TEST(generic_typeparam_named_set_index_7203,
           set_named_param_bound_to_array_string_reads_first_element) {
    BEZEL_EXPECT_EQ(runAsanOut(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var a: Array<string> := [\"ab\", \"cd\"]; "
        "    Console.WriteLine(F<Array<string>>(a)); "
        "    return 0; "
        "  end Main; "
        "  public static method F<Set>(x: Set): string "
        "    var v: string := x[0]; "
        "    return v; "
        "  end F; "
        "end C;"), "ab");
}

// spec §3.6 rule 2 / §14.1 (v0.343): the `T`-NAMED spelling — the type
// parameter that names no collection at all. This row pins the scope of the
// fix: the receiver's shape is read from the BOUND for EVERY type parameter,
// so a `T` bound to `Array<int>` reads the FIRST element, 7. On this lane's
// base the same program printed 0 — the read silently degraded, a wrong
// answer, because the head name `T` names no collection.
BEZEL_TEST(generic_typeparam_named_set_index_7203,
           t_named_param_bound_to_array_int_reads_first_element) {
    BEZEL_EXPECT_EQ(runAsanOut(matrixSrc("T", "Array<int>", "[7, 8, 9]")), "7");
}

// spec §3.6 rule 2 (v0.343): the CRASH MATRIX, array-backed bounds. `Stack`
// and `Queue` are `bz_array_t*`-backed (stdlib §4.11), so a `Set`-named or a
// `T`-named parameter bound to one reads the first element. Pre-fix the
// `Set`-named spellings sized the slot from the HEAD (8 bytes) into a 4-byte
// slot (ASan: stack-buffer-overflow) and the `T`-named spellings degraded to
// 0.
BEZEL_TEST(generic_typeparam_named_set_index_7203,
           set_named_param_bound_to_stack_int_reads_first_element) {
    BEZEL_EXPECT_EQ(runAsanOut(matrixSrc("Set", "Stack<int>", "[7, 8, 9]")),
                    "7");
}

BEZEL_TEST(generic_typeparam_named_set_index_7203,
           set_named_param_bound_to_queue_int_reads_first_element) {
    BEZEL_EXPECT_EQ(runAsanOut(matrixSrc("Set", "Queue<int>", "[7, 8, 9]")),
                    "7");
}

BEZEL_TEST(generic_typeparam_named_set_index_7203,
           t_named_param_bound_to_stack_int_reads_first_element) {
    BEZEL_EXPECT_EQ(runAsanOut(matrixSrc("T", "Stack<int>", "[7, 8, 9]")), "7");
}

BEZEL_TEST(generic_typeparam_named_set_index_7203,
           t_named_param_bound_to_queue_int_reads_first_element) {
    BEZEL_EXPECT_EQ(runAsanOut(matrixSrc("T", "Queue<int>", "[7, 8, 9]")), "7");
}

// stdlib §4.10 / §4.18 (v0.343): the DICT-BACKED bound. `Set<T>` is a
// hash-based set with no indexer, so the collection-indexer path must
// DECLINE it (spec §23.4 defines an indexer as "a property whose name is
// replaced by `[parameters]`") rather than read the hash table as the dense
// array the path assumes. The read degrades to 0 — the way a `T`-named
// parameter already degrades — and NO shape may crash: the `Set`-named
// spelling overflowed the stack slot on this lane's base (ASan:
// stack-buffer-overflow, WRITE of size 8), and the `T`-named spelling read a
// `bz_dict_t*` as a `bz_array_t*` (ASan: SEGV on a read at the dictionary's
// first bucket).
BEZEL_TEST(generic_typeparam_named_set_index_7203,
           set_named_param_bound_to_dict_backed_set_int_does_not_crash) {
    BEZEL_EXPECT_EQ(runAsanOut(matrixSrc("Set", "Set<int>", "[1, 2, 3]")), "0");
}

BEZEL_TEST(generic_typeparam_named_set_index_7203,
           t_named_param_bound_to_dict_backed_set_int_does_not_crash) {
    BEZEL_EXPECT_EQ(runAsanOut(matrixSrc("T", "Set<int>", "[1, 2, 3]")), "0");
}

// spec §14.2 / §23.4 (v0.343): THE LEGAL PROGRAM THE FIX MUST NOT REJECT.
// A type parameter named `Set` CONSTRAINED to an indexer-bearing interface
// still reads its element: 40 + 2 = 42. (The #7114 suppression deliberately
// lets a declared generic parameter's head reach the collection path, and the
// #794 constraint devirtualization resolves the receiver through the same
// substitution — neither may be disturbed by #7203.)
BEZEL_TEST(generic_typeparam_named_set_index_7203,
           set_named_param_constrained_to_indexable_reads_its_element) {
    BEZEL_EXPECT_EQ(runAsanOut(
        "module M; "
        "interface IIndexable readonly property [i: int]: int; end IIndexable; "
        "class SingleInt: IIndexable "
        "  private _v: int; "
        "  public constructor(v: int) _v := v; end constructor; "
        "  public property [i: int]: int => _v + i; "
        "end SingleInt; "
        "class C "
        "  public static method At<Set>(x: Set, i: int): int "
        "      where Set: IIndexable "
        "    return x[i]; "
        "  end At; "
        "  public static method Main(): int "
        "    using var s := new SingleInt(40); "
        "    Console.WriteLine(At<SingleInt>(s, 2)); "
        "    return 0; "
        "  end Main; "
        "end C;"), "42");
}
