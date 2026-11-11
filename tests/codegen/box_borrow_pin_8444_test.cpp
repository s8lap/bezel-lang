// Codegen — #8444 (§28.9 / §8.6, v0.357): a local (or an argument) read from a
// box-storage `Object` field dangled once the field was overwritten.
//
// THE RULE. §28.9, *Boxed value types*: "created implicitly when a value type
// is used where `Object` or another reference type is expected ... a boxed
// value is an immutable shared reference". A box is refcounted by the runtime,
// and every holder of a shared reference keeps it alive for as long as it
// holds it.
//
// THE DEFECT. A box-storage field (the box-only fields of #8344 and the
// dynamic ones of #8215) releases the box it holds when a store displaces it
// and when its holder is disposed. A LOCAL bound from a read of such a field
// (`var t: Object? := h.o;`), an ARGUMENT (`Use(h.o, h)`) and a call RESULT
// that forwards the field (`var t: Object? := h.Get();`) took no reference of
// their own, so the displaced box was freed while they still named it: the
// issue's repro printed three BLANK lines instead of `0 1 2`. ASan does not see
// it (the freed block is read only inside the uninstrumented runtime helpers);
// the wrong VALUE is the symptom, so every row here pins stdout.
//
// THE FIX. The box-borrow pin (`Emitter::armBoxBorrowPin` /
// `rebindBoxBorrowPin` / `pinBoxBorrowArgument`): a binding whose source has
// unknown box provenance tests the value once, at the bind, with the #8215
// run-time discriminator and, iff it is a box, retains it; the reference is
// released at the local's scope end (or when the local is rebound), and for an
// argument when the call returns or unwinds.
//
// ORACLE. `runAotChecked` (generated-code ASan + LeakSanitizer): a leak or a
// double release the pin could introduce turns into `ran == false`, and the
// stdout pin catches the use-after-free ASan is blind to. Every row but the
// issue's literal three-iteration program loops its shape 200 times.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The classes every row shares. `Holder.o` is BOX-ONLY when `kHolderBoxOnly`
// is used (every store is a box or null) and DYNAMIC with `kHolderDynamic`
// (`Put` stores an `Object`-typed value).
#define BZ8444_HOLDER_COMMON                                                   \
    "  public o: Object?; "                                                    \
    "  public constructor() end constructor; "                                 \
    "  public method Take(n: int) o := n; end Take; "                          \
    "  public method Clear() o := null; end Clear; "                           \
    "  public method Get(): Object? return o; end Get; "                       \
    "  public method Swap(n: int): int "                                       \
    "    var t: Object? := o; "                                                \
    "    o := n; "                                                             \
    "    return (t as int) ?? -1; "                                            \
    "  end Swap; "

#define BZ8444_HOLDER_BOX_ONLY                                                 \
    "public class Holder " BZ8444_HOLDER_COMMON "end Holder; "

#define BZ8444_HOLDER_DYNAMIC                                                  \
    "public class Holder " BZ8444_HOLDER_COMMON                                \
    "  public method Put(x: Object?) o := x; end Put; "                        \
    "end Holder; "

#define BZ8444_MAIN_OPEN                                                       \
    "  public static method Main(): int "                                      \
    "    var sum := 0; "                                                       \
    "    for var i in 0..<200 do "

#define BZ8444_MAIN_CLOSE                                                      \
    "    end for; "                                                            \
    "    Console.WriteLine(sum); "                                             \
    "    return 0; "                                                           \
    "  end Main; "                                                             \
    "end Program;"

// Row 1 -- the issue's repro, verbatim (module P5B).
const char* kIssueRepro =
    "module P5B; "
    "public class Holder "
    "  public o: Object?; "
    "  public constructor() end constructor; "
    "  public method Take(n: int) o := n; end Take; "
    "end Holder; "
    "public class Program "
    "  public static method Main(): int "
    "    for var i in 0..<3 do "
    "      using var h := new Holder(); "
    "      h.Take(i); "
    "      var t: Object? := h.o; "
    "      h.Take(5000); "
    "      Console.WriteLine(t?.ToString() ?? \"null\"); "
    "    end for; "
    "    return 0; "
    "  end Main; "
    "end Program;";

// Row 2 -- the issue's own looped form (`(t as int) ?? -1`), box-only field,
// displaced by a store inside a method.
const char* kLocalBoxOnlyMethodStore =
    "module M8444B; "
    BZ8444_HOLDER_BOX_ONLY
    "public class Program "
    BZ8444_MAIN_OPEN
    "      using var h := new Holder(); "
    "      h.Take(i); "
    "      var t: Object? := h.o; "
    "      h.Take(5000); "
    "      sum += (t as int) ?? -1; "
    BZ8444_MAIN_CLOSE;

// Row 3 -- dynamic field, displaced by a DIRECT assignment to the field.
const char* kLocalDynamicDirectStore =
    "module M8444C; "
    BZ8444_HOLDER_DYNAMIC
    "public class Program "
    BZ8444_MAIN_OPEN
    "      using var h := new Holder(); "
    "      h.Take(i); "
    "      var t: Object? := h.o; "
    "      h.o := 5000; "
    "      sum += (t as int) ?? -1; "
    BZ8444_MAIN_CLOSE;

// Row 4 -- the local is ASSIGNED (not initialized) from the field, and the
// field is CLEARED (`o := null` releases the displaced box of a dynamic field).
const char* kAssignedLocalFieldCleared =
    "module M8444D; "
    BZ8444_HOLDER_DYNAMIC
    "public class Program "
    BZ8444_MAIN_OPEN
    "      using var h := new Holder(); "
    "      h.Take(i); "
    "      var t: Object? := null; "
    "      t := h.o; "
    "      h.Clear(); "
    "      sum += (t as int) ?? -1; "
    BZ8444_MAIN_CLOSE;

// Row 5 -- the PARAMETER shape: the callee displaces the box its own
// parameter names.
const char* kParameter =
    "module M8444E; "
    BZ8444_HOLDER_BOX_ONLY
    "public class Program "
    "  static method Use(x: Object?, h: Holder): int "
    "    h.Take(5000); "
    "    return (x as int) ?? -1; "
    "  end Use; "
    BZ8444_MAIN_OPEN
    "      using var h := new Holder(); "
    "      h.Take(i); "
    "      sum += Program.Use(h.o, h); "
    BZ8444_MAIN_CLOSE;

// Row 6 -- the RETURN shape: a method forwards the field, the caller binds
// the result.
const char* kReturn =
    "module M8444F; "
    BZ8444_HOLDER_DYNAMIC
    "public class Program "
    BZ8444_MAIN_OPEN
    "      using var h := new Holder(); "
    "      h.Take(i); "
    "      var t: Object? := h.Get(); "
    "      h.Take(5000); "
    "      sum += (t as int) ?? -1; "
    BZ8444_MAIN_CLOSE;

// Row 7 -- the alias OUTLIVES the holder: the holder's DISPOSER releases the
// field's reference.
const char* kAliasOutlivesHolder =
    "module M8444G; "
    BZ8444_HOLDER_BOX_ONLY
    "public class Program "
    BZ8444_MAIN_OPEN
    "      var t: Object? := null; "
    "      do "
    "        using var h := new Holder(); "
    "        h.Take(i); "
    "        t := h.o; "
    "      end; "
    "      sum += (t as int) ?? -1; "
    BZ8444_MAIN_CLOSE;

// Row 8 -- the read is the implicit-`this` field inside the holder's own
// method (`Swap`: `var t: Object? := o; o := n;`).
const char* kImplicitThisRead =
    "module M8444H; "
    BZ8444_HOLDER_BOX_ONLY
    "public class Program "
    BZ8444_MAIN_OPEN
    "      using var h := new Holder(); "
    "      h.Take(i); "
    "      sum += h.Swap(7); "
    BZ8444_MAIN_CLOSE;

// Row 9 -- the local is rebound in a loop (each rebind releases the previous
// pin), then the last box is displaced.
const char* kRebindInLoop =
    "module M8444I; "
    BZ8444_HOLDER_DYNAMIC
    "public class Program "
    BZ8444_MAIN_OPEN
    "      using var h := new Holder(); "
    "      var t: Object? := null; "
    "      for var j in 0..<4 do "
    "        h.Take(i + j); "
    "        t := h.o; "
    "      end for; "
    "      h.Take(5000); "
    "      sum += ((t as int) ?? -1) - 3; "
    BZ8444_MAIN_CLOSE;

// Row 10 -- THE MIRROR: the box is NEVER displaced. Every pinned shape (local,
// assigned local, parameter, return, implicit-this) runs against a box that
// stays in its field, beside a class instance and a string in the same kind of
// slot: the pin must add neither a leak nor a second release, and must leave a
// non-box alone.
const char* kMirrorNeverDisplaced =
    "module M8444J; "
    BZ8444_HOLDER_DYNAMIC
    "public class Leaf "
    "  public v: int; "
    "  public constructor(v: int) this.v := v; end constructor; "
    "end Leaf; "
    "public class Program "
    "  static method Peek(x: Object?): int "
    "    return (x as int) ?? ((x as Leaf)?.v ?? ((x as string)?.Length ?? -1)); "
    "  end Peek; "
    BZ8444_MAIN_OPEN
    "      using var h := new Holder(); "
    "      h.Take(i); "
    "      var a: Object? := h.o; "
    "      var b: Object? := null; "
    "      b := h.o; "
    "      var c: Object? := h.Get(); "
    "      var d: Object? := a; "
    "      sum += Program.Peek(h.o) + Program.Peek(a) + Program.Peek(b) "
    "           + Program.Peek(c) + Program.Peek(d) - 4 * i; "
    "      using var leaf := new Leaf(i); "
    "      using var k := new Holder(); "
    "      k.Put(leaf); "
    "      var e: Object? := k.o; "
    "      sum += Program.Peek(k.o) + Program.Peek(e) - 2 * i; "
    "      k.Put(\"ab\"); "
    "      var f: Object? := k.o; "
    "      sum += Program.Peek(k.o) + Program.Peek(f) - 4; "
    BZ8444_MAIN_CLOSE;

// Row 11 -- UNWIND: the callee displaces the box and then RAISES, and a local
// pin is live across a `throw`. The pins are released on the exception path
// (a leak here is `ran == false`).
const char* kUnwind =
    "module M8444K; "
    BZ8444_HOLDER_BOX_ONLY
    "public class Program "
    "  static method Boom(x: Object?, h: Holder, displace: bool): int "
    "    if displace then h.Take(5000); end if; "
    "    if ((x as int) ?? -1) >= 0 then "
    "      throw new InvalidOperationException(\"b\"); "
    "    end if; "
    "    return 0; "
    "  end Boom; "
    BZ8444_MAIN_OPEN
    "      using var h := new Holder(); "
    "      h.Take(i); "
    "      try "
    "        sum += Program.Boom(h.o, h, i mod 2 = 0); "
    "        sum += 100000; "
    "      catch "
    "        InvalidOperationException => sum += 1; "
    "      end try; "
    "      h.Take(i); "
    "      try "
    "        var t: Object? := h.o; "
    "        if i mod 2 = 0 then h.Take(5000); end if; "
    "        if ((t as int) ?? -1) = i then "
    "          throw new InvalidOperationException(\"x\"); "
    "        end if; "
    "        sum += 100000; "
    "      catch "
    "        InvalidOperationException => sum += i - 1; "
    "      end try; "
    BZ8444_MAIN_CLOSE;

void expectCleanSum(const JitResult& r, const char* expected) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// FAIL-WITHOUT: prints three blank lines (`t` reads the freed box).
BEZEL_TEST(codegen_box_borrow_pin_8444,
           spec_28_9_issue_repro_local_survives_field_overwrite) {
    JitResult r = runAot(kIssueRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "0\n1\n2\n");
}

// FAIL-WITHOUT (rows 2-9): prints -200 (every read is of a freed box).
BEZEL_TEST(codegen_box_borrow_pin_8444,
           spec_28_9_local_box_only_field_method_store) {
    expectCleanSum(runAotChecked(kLocalBoxOnlyMethodStore), "19900\n");
}

BEZEL_TEST(codegen_box_borrow_pin_8444,
           spec_28_9_local_dynamic_field_direct_store) {
    expectCleanSum(runAotChecked(kLocalDynamicDirectStore), "19900\n");
}

BEZEL_TEST(codegen_box_borrow_pin_8444,
           spec_28_9_assigned_local_field_cleared) {
    expectCleanSum(runAotChecked(kAssignedLocalFieldCleared), "19900\n");
}

BEZEL_TEST(codegen_box_borrow_pin_8444,
           spec_28_9_parameter_callee_displaces_the_box) {
    expectCleanSum(runAotChecked(kParameter), "19900\n");
}

BEZEL_TEST(codegen_box_borrow_pin_8444,
           spec_28_9_return_forwarding_the_field) {
    expectCleanSum(runAotChecked(kReturn), "19900\n");
}

BEZEL_TEST(codegen_box_borrow_pin_8444,
           spec_28_9_alias_outlives_the_holder_disposal) {
    expectCleanSum(runAotChecked(kAliasOutlivesHolder), "19900\n");
}

BEZEL_TEST(codegen_box_borrow_pin_8444,
           spec_28_9_implicit_this_field_read) {
    expectCleanSum(runAotChecked(kImplicitThisRead), "19900\n");
}

BEZEL_TEST(codegen_box_borrow_pin_8444,
           spec_28_9_local_rebound_in_loop) {
    expectCleanSum(runAotChecked(kRebindInLoop), "19900\n");
}

// CONTROL (green on the base too): nothing is displaced, so the pin must be
// invisible -- no leak, no double release, a non-box untouched.
BEZEL_TEST(codegen_box_borrow_pin_8444,
           spec_28_9_mirror_never_displaced_box_class_string) {
    expectCleanSum(runAotChecked(kMirrorNeverDisplaced), "19900\n");
}

// FAIL-WITHOUT: the displaced half reads a freed box (the sum is wrong). On
// the fix the exception path releases both pins.
BEZEL_TEST(codegen_box_borrow_pin_8444,
           spec_28_9_pins_released_on_unwind) {
    expectCleanSum(runAotChecked(kUnwind), "19900\n");
}
