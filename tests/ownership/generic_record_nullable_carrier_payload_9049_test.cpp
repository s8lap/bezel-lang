// Spec-derived regression tests for issue #9049 — a `T?` carrier built over a
// GENERIC RECORD (or CHOICE) value inside a generic class emitted malformed IR.
//
// THE RULE. A nullable VALUE type lowers to a tagged carrier `{i8 hasValue,
// T}` (spec §7.2 / §9.4, stdlib §4.8's `V?`). The carrier's PAYLOAD must be the
// SAME LLVM type as the value it carries — one type reference, one lowering.
// §14.1 (v0.359) binds a generic type's parameters to the type ARGUMENTS at
// each instantiation, and an UNSPECIALIZED template body keeps the ERASED
// struct for a generic record whose arguments are still unbound template
// parameters (the template body must share its base struct with every other
// use of that type).
//
// WHAT WAS BROKEN. `Emitter::nullableValueStructTy` (src/codegen/
// LlvmBackend_Layout.cpp) re-derived the payload struct ITSELF through
// `RecordSubstGuard`, which ALWAYS specialises a generic record/choice — while
// the VALUE of the same type was lowered through `lowerTypeRef`, which keeps
// the ERASED struct when every type argument is still unbound. In the
// UNSPECIALIZED template body of `class Cache<V>` the two disagreed: the
// carrier's payload was the SPECIALIZED struct and the value the UNSPECIALIZED
// one, and the `insertvalue` that builds the carrier was rejected by LLVM's
// verifier.
//
// MEASURED on the base (r30-c1 @ ed5af90fa): the issue's program
// `Dictionary<string, Entry<V>>.GetOrNull(k)` inside `class Cache<V>` failed
// with
//
//   Invalid InsertValueInst operands!
//     %14 = insertvalue { i8, %"record.Entry$spec0" } %13, %record.Entry %gon.val, 1
//   bezelc: codegen error: IR verification failed (malformed IR emitted; see
//   report above)
//
// The same mismatch reached every other site that builds a `T?` carrier from a
// collection value at a generic record: the §5 LINQ `*OrNull` family over
// `Array<Entry<V>>` (`%on.val`), a two-level generic dictionary, and the
// generic CHOICE twin (`Dictionary<string, Opt<V>>.GetOrNull`). The fix reads
// the carrier's payload from the ONE authority (`lowerTypeRef` on the
// non-nullable form — the shape the `DictionaryEntry` carrier already used),
// so the carrier and the value are the same type by construction.
//
// Every row below is on `runAot` and is RED on the base: each program fails to
// COMPILE there (the IR-verification error above), so `r.ran` is false.
//
// The programs keep the dictionary a LOCAL of the method rather than the
// issue's FIELD: the issue's own shape stores a boxed record value in a
// `Dictionary` FIELD, and that field's disposal leaks the box (8 bytes per
// stored value) — a PRE-EXISTING defect of the field-disposal path, unrelated
// to this one and filed separately. The lowering under test is the template
// body's, which the local shape exercises identically; the field shape would
// only add that unrelated leak to the mandatory ASan sweep.
//
// The two green rows are the suite's boundary CONTROLS: the issue's own
// non-generic twin (which the issue says already works) and the generic-class
// / non-generic-record twin (the class's substitution is active, but the
// record is not generic, so its carrier is the plain struct). Both are
// base-green by construction and are kept here because a corpus case cannot
// express the non-generic twin's runtime value (the two controls the lane
// contract allows).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The program compiled, ran to completion with the expected exit code, and
// printed exactly the expected output. On the base every one of these
// programs fails to COMPILE (`r.ran` is false, `r.stdoutText` is empty), so
// the row fails there.
void expectRan(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// RED on the base — THE ISSUE'S SHAPE: a generic class over a generic record
// whose `Dictionary<string, Entry<V>>` value is read with `GetOrNull`
// (stdlib §4.8's `GetOrNull(key: K): V?` — the §1 `*OrNull` tier: absent ->
// null, present -> the value). The carrier is built over the generic record's
// value, so the carrier's payload and the value must be the same type.
//
// On the base: "Invalid InsertValueInst operands! … %record.Entry %gon.val".
// Here: the value reads back and the program prints `10`.
BEZEL_TEST(ownership_generic_record_nullable_carrier_payload_9049,
           dictionary_get_or_null_over_a_generic_record_value_spec_14_1) {
    auto r = runAot(R"bzl(module M;
public record Entry<V>(Value: V, Expires: int);
public class Cache<V>
  public method ExpiresOr(k: string, v: V): int
    var d: Dictionary<string, Entry<V>> := new Dictionary<string, Entry<V>>();
    d[k] := new Entry<V>(v, 10);
    var e := d.GetOrNull(k);
    return (if e = null then -1 else e.Expires);
  end ExpiresOr;
end Cache;
public class Program
  public static method Main()
    using var c := new Cache<int>();
    Console.WriteLine(c.ExpiresOr("a", 1));
  end Main;
end Program;
)bzl");
    expectRan(r, "10\n");
}

// RED on the base — THE SIBLING FAMILY: stdlib §5's `*OrNull` element accessors
// over an `Array<Entry<V>>` (`FirstOrNull` / `LastOrNull` /
// `ElementAtOrNull`) build the same `T?` carrier over the same generic record
// value (`%on.val` on the base).
//
// Here: all three read the stored entry's `Expires` (10).
BEZEL_TEST(ownership_generic_record_nullable_carrier_payload_9049,
           array_or_null_family_over_a_generic_record_value_spec_5_1_4_8) {
    auto r = runAot(R"bzl(module M;
public record Entry<V>(Value: V, Expires: int);
public class Cache<V>
  private owned _a: Array<Entry<V>> := new Array<Entry<V>>();
  public method Add(v: V) => _a.Add(new Entry<V>(v, 10));
  public method FirstExpires(): int
    var e := _a.FirstOrNull();
    return (if e = null then -1 else e.Expires);
  end FirstExpires;
  public method LastExpires(): int
    var e := _a.LastOrNull();
    return (if e = null then -1 else e.Expires);
  end LastExpires;
  public method AtExpires(i: int): int
    var e := _a.ElementAtOrNull(i);
    return (if e = null then -1 else e.Expires);
  end AtExpires;
end Cache;
public class Program
  public static method Main()
    using var c := new Cache<int>();
    c.Add(1);
    Console.WriteLine(c.FirstExpires());
    Console.WriteLine(c.LastExpires());
    Console.WriteLine(c.AtExpires(0));
  end Main;
end Program;
)bzl");
    expectRan(r, "10\n10\n10\n");
}

// RED on the base — A TWO-LEVEL GENERIC: the value type of the outer
// `Dictionary` is itself a generic instantiation (`Dictionary<string,
// Entry<V>>`), and `GetOrNull` is called on it. The carrier is built over the
// generic record at the INNER level.
//
// Here: the inner value reads back and the program prints `10`.
BEZEL_TEST(ownership_generic_record_nullable_carrier_payload_9049,
           two_level_generic_dictionary_get_or_null_spec_14_1) {
    auto r = runAot(R"bzl(module M;
public record Entry<V>(Value: V, Expires: int);
public class Cache<V>
  public method ExpiresOr(k: string, k2: string, v: V): int
    var inner: Dictionary<string, Entry<V>> := new Dictionary<string, Entry<V>>();
    inner[k2] := new Entry<V>(v, 10);
    var outer: Dictionary<string, Dictionary<string, Entry<V>>> := new Dictionary<string, Dictionary<string, Entry<V>>>();
    outer[k] := inner;
    var got := outer.GetOrNull(k);
    if got = null then return -1; end if;
    var e := got!.GetOrNull(k2);
    return (if e = null then -1 else e!.Expires);
  end ExpiresOr;
end Cache;
public class Program
  public static method Main()
    using var c := new Cache<int>();
    Console.WriteLine(c.ExpiresOr("a", "b", 1));
  end Main;
end Program;
)bzl");
    expectRan(r, "10\n");
}

// RED on the base — THE GENERIC CHOICE TWIN: `nullableValueStructTy`'s choice
// branch re-derived its payload the same way the record branch did, so a
// `Dictionary<string, Opt<V>>.GetOrNull` failed the verifier the same way
// (`%choice.Opt$spec0` against `%choiceOpt`). Spec §35.4's per-instantiation
// classification and §14.1's substitution bind the choice's payload type at
// each instantiation, and the carrier's payload must be the value's type.
//
// Here: the stored `Some` payload is read back and the program prints `1`.
BEZEL_TEST(ownership_generic_record_nullable_carrier_payload_9049,
           generic_choice_value_get_or_null_spec_35_4_14_1) {
    auto r = runAot(R"bzl(module M;
public choice Opt<V> Some(v: V), None; end Opt;
public class Cache<V>
  public method ExpiresOr(k: string, v: V): int
    var d: Dictionary<string, Opt<V>> := new Dictionary<string, Opt<V>>();
    d[k] := Opt<V>.Some(v);
    var o := d.GetOrNull(k);
    return (case o of Some(var x) => 1, _ => -1);
  end ExpiresOr;
end Cache;
public class Program
  public static method Main()
    using var c := new Cache<int>();
    Console.WriteLine(c.ExpiresOr("a", 1));
  end Main;
end Program;
)bzl");
    expectRan(r, "1\n");
}

// RED on the base — A REFERENCE-TYPED type argument: `Cache<string>` puts a
// `string` in the generic record's field, so the record's layout at the
// instantiation is a POINTER where the ERASED template layout is the
// placeholder. The carrier's payload and the value must still be the SAME
// type — a fix that made the two sides differ would read the stored value
// through the wrong layout.
//
// Here: the stored entry's `Expires` reads back and the program prints `10`.
BEZEL_TEST(ownership_generic_record_nullable_carrier_payload_9049,
           dictionary_get_or_null_over_a_reference_typed_generic_record_value_spec_14_1) {
    auto r = runAot(R"bzl(module M;
public record Entry<V>(Value: V, Expires: int);
public class Cache<V>
  public method ExpiresOr(k: string, v: V): int
    var d: Dictionary<string, Entry<V>> := new Dictionary<string, Entry<V>>();
    d[k] := new Entry<V>(v, 10);
    var e := d.GetOrNull(k);
    return (if e = null then -1 else e.Expires);
  end ExpiresOr;
end Cache;
public class Program
  public static method Main()
    using var c := new Cache<string>();
    Console.WriteLine(c.ExpiresOr("a", "hello"));
  end Main;
end Program;
)bzl");
    expectRan(r, "10\n");
}

// CONTROL 1 — THE ISSUE'S OWN NON-GENERIC TWIN ("The same code in a
// non-generic context (`Main`) works"): the carrier's payload and the value
// are the plain struct, so nothing changes there. Base-green by construction.
BEZEL_TEST(ownership_generic_record_nullable_carrier_payload_9049,
           non_generic_twin_get_or_null_still_reads_the_value_spec_4_8) {
    auto r = runAot(R"bzl(module M;
public record Entry(Value: int, Expires: int);
public class Cache
  public method ExpiresOr(k: string, v: int): int
    var d: Dictionary<string, Entry> := new Dictionary<string, Entry>();
    d[k] := new Entry(v, 10);
    var e := d.GetOrNull(k);
    return (if e = null then -1 else e.Expires);
  end ExpiresOr;
end Cache;
public class Program
  public static method Main()
    using var c := new Cache();
    Console.WriteLine(c.ExpiresOr("a", 1));
  end Main;
end Program;
)bzl");
    expectRan(r, "10\n");
}

// CONTROL 2 — A GENERIC CLASS over a NON-GENERIC record: the class's
// substitution is active, but the record has no type parameters, so its
// carrier's payload is the plain struct and the value is the plain struct.
// The fix must not change this shape. Base-green by construction.
BEZEL_TEST(ownership_generic_record_nullable_carrier_payload_9049,
           generic_class_non_generic_record_value_still_reads_the_value_spec_14_1) {
    auto r = runAot(R"bzl(module M;
public record Entry(Value: int, Expires: int);
public class Cache<V>
  public method ExpiresOr(k: string, v: V): int
    var d: Dictionary<string, Entry> := new Dictionary<string, Entry>();
    d[k] := new Entry(10, 10);
    var e := d.GetOrNull(k);
    return (if e = null then -1 else e.Expires);
  end ExpiresOr;
end Cache;
public class Program
  public static method Main()
    using var c := new Cache<int>();
    Console.WriteLine(c.ExpiresOr("a", 1));
  end Main;
end Program;
)bzl");
    expectRan(r, "10\n");
}
