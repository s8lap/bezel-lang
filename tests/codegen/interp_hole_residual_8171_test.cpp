// Disposal-correctness — the RESIDUAL interpolation-hole shapes that still
// leaked their owned operand after #8143 (issue #8171; spec §18.5, §28.3.11,
// §28.3 K7.1, §28.9, stdlib §3.1 / §9.6 — all v0.355).
//
// §28.3.11 (v0.355), the rule the issue cites: an unbound owned temporary
// "used only by *borrowing* operations within the expression" is "disposed at
// the **end of the full expression** in which it is produced, after every use
// of it within that expression has completed".
//
// §18.5: an interpolation hole BORROWS its operand — the hole dispatches the
// value's `ToString` and the rendered bytes are copied into the concatenation.
// So a FRESH OWNED PRODUCER in the hole position hands its +1 to nobody, and
// the hole expression owes the disposal.
//
// #8143 installed that discharge for ONE producer shape — `new C(...)` and an
// `owned C`-returning call of a heap CLASS (`isFreshOwnedClassProducer`).
// The rows below are the shapes that predicate cannot reach:
//
//   * an OWNED MEET (§28.3 K7.1 — "owned ⊓ owned = owned"): `a ?? b`, an
//     if-expression and a case-expression whose arms are all owned producers
//     yield a fresh owned VALUE, which the hole borrows;
//   * a DECLARATION-LESS built-in producer: `new StringBuilder(...)`
//     (stdlib §3.1) and the `owned Array<T>` materializers `s.Split(...)` /
//     `s.ToCharArray()` (stdlib §9.6). Every built-in is declaration-less, so
//     the class test in the shared predicate can never fire for them — and
//     their RELEASE is not the vtable Dispose either (a `bz_array_t*` and a
//     `StringBuilder` have no Bezel vtable at word 0, so a vtable dispatch
//     would be a wild jump, not a reclaim).
//
// Each row is on the checked oracle (`runAotChecked` — ASan/LSan): a missed
// boundary disposal is a DIRECT leak (the LSan report flips `r.ran` to false)
// and a reclaim that wrongly claimed a BORROWED operand would be a
// double-free / use-after-free (also flipping `r.ran` to false).
//
// The rows are 100x loops (the issue's amplification) so a leak is 100
// allocations — LSan reports the total, and the per-evaluation destructor
// marker count pins "disposed exactly once per evaluation" from the other
// side. The rendered OUTPUT is asserted EXACTLY (the whole stdout), so a row
// cannot pass by rendering the wrong text.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"  // runAotChecked / runAot — AOT oracles
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// `C` — a class with a per-instance destructor marker, so the destructor's
// OUTPUT counts the disposals. It declares NO `ToString`, so §8.5's class
// default renders it as its qualified name ("M.C").
//
// `Make` — the static factory (`owned C`). `MakeN` — the nullable factory
// (`owned C?`), the LHS arm of the `??` meet: it yields `null` for an even
// index and a fresh `owned C` (id 4) for an odd one.
const char* kPrelude = R"(
module M;
public class C
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
  destructor Console.WriteLine("D" + Id.ToString()); end destructor;
  public static method Make(): owned C
    return new C(1);
  end Make;
  public static method MakeN(k: int): owned C?
    if k mod 2 = 0 then return null; end if;
    return new C(4);
  end MakeN;
end C;
)";

// `T` — a class whose `ToString` THROWS (the #8172 shape). Its destructor
// marker ("DT") counts the disposals of the `T` temporary, and the fact that
// the marker appears at all is the assertion that the temporary was
// discharged on the EXCEPTION edge (§28.3's *Exception-path discharge*).
const char* kThrowPrelude = R"(
module M;
public class C
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
  destructor Console.WriteLine("D" + Id.ToString()); end destructor;
  public static method Make(): owned C
    return new C(1);
  end Make;
end C;
public class T
  public constructor() end constructor;
  destructor Console.WriteLine("DT"); end destructor;
  public override method ToString(): string
    throw new InvalidOperationException("render");
  end ToString;
  public static method Make(): owned T
    return new T();
  end Make;
end T;
)";

} // namespace

// §28.3.11 / §28.3 K7.1 (v0.355) — the `??` MEET row:
// `$"{C.MakeN(i) ?? C.Make()}"`.
//
// The meet's RESULT is a fresh owned value (§28.3 K7.1: "owned ⊓ owned =
// owned"); whichever arm ran, that value is what the hole borrows. Without the
// discharge it leaks 100 x 16 B (LSan fires → r.ran is false) and the
// destructor's marker never appears.
//
// The stdout is asserted EXACTLY: one render then one destructor marker per
// evaluation, alternating between the two arms (an even index takes the
// `C.Make()` arm → id 1 → "D1"; an odd index takes the `C.MakeN(i)` arm →
// id 4 → "D4").
BEZEL_TEST(interp_hole_residual_8171,
           coalesce_meet_hole_disposed_at_full_expression_boundary) {
    auto r = runAotChecked(std::string(kPrelude) + R"(
public class Program
  public static method Main(): int
    var i := 0;
    while i < 100 do
      Console.WriteLine($"{C.MakeN(i) ?? C.Make()}");
      i := i + 1;
    end while;
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string expected;
    for (int k = 0; k < 50; ++k) {
        expected += "M.C\nD1\n";
        expected += "M.C\nD4\n";
    }
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// §28.3.11 / §28.3 K7.1 (v0.355) — the `case` MEET row:
// `$"{(case i mod 2 of 0 => C.Make(), _ => new C(5))}"`.
//
// The same obligation through the OTHER meet spelling: exactly one arm runs,
// and its value (a fresh owned `C`) is what the hole borrows. A `case` arm
// that is a `new` and an arm that is an `owned`-returning call are BOTH owned
// producers, so the meet is owned.
BEZEL_TEST(interp_hole_residual_8171,
           case_meet_hole_disposed_at_full_expression_boundary) {
    // The `case` spelling of the same meet — its own row because the two
    // lowers are different code paths (the `case` result is a PHI over the
    // arms, the `??` result a PHI over the two operands). The arms are a
    // `new` and an `owned`-returning call — BOTH owned producers, so the meet
    // is owned.
    auto r = runAotChecked(std::string(kPrelude) + R"(
public class Program
  public static method Main(): int
    var i := 0;
    while i < 100 do
      Console.WriteLine($"{(case i mod 2 of 0 => C.Make(), _ => new C(5))}");
      i := i + 1;
    end while;
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string expected;
    for (int k = 0; k < 50; ++k) {
        expected += "M.C\nD1\n";
        expected += "M.C\nD5\n";
    }
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// §28.3.11 / stdlib §3.1 (v0.355) — the `new StringBuilder(...)` row.
//
// A `StringBuilder` is a trivial-dispose, vtable-LESS built-in (stdlib §3.1):
// its release is `emitVtablelessBuiltinRelease` (`bz_array_free` plus the
// string-element release thunk for its parts), NOT a vtable Dispose — a
// `StringBuilder` has no Bezel vtable at word 0.
//
// The rendered text is the builder's own contents ("sb"), so the stdout is
// exactly 100 lines and is asserted verbatim.
BEZEL_TEST(interp_hole_residual_8171,
           string_builder_hole_disposed_at_full_expression_boundary) {
    auto r = runAotChecked(std::string(kPrelude) + R"(
public class Program
  public static method Main(): int
    var i := 0;
    while i < 100 do
      Console.WriteLine($"{new StringBuilder("sb")}");
      i := i + 1;
    end while;
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string expected;
    for (int k = 0; k < 100; ++k) expected += "sb\n";
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// §28.3.11 / stdlib §9.6 (v0.355) — the `s.Split(...)` row:
// `$"{s.Split(',')}"`.
//
// `Split` returns `owned Array<string>` (stdlib §9.6): the result is a fresh
// ARRAY BACKING the hole borrows, and its release is the runtime free plus
// the §28.11 element disposer (`bz_array_free` with the string-release thunk
// for the `Array<string>` elements) — never a vtable Dispose.
//
// The array renders as its type name (§8.5's default — an `Array` declares no
// `ToString`), so the stdout is exactly 100 lines and is asserted verbatim.
BEZEL_TEST(interp_hole_residual_8171,
           split_array_hole_disposed_at_full_expression_boundary) {
    auto r = runAotChecked(std::string(kPrelude) + R"(
public class Program
  public static method Main(): int
    var s := "a,b,c";
    var i := 0;
    while i < 100 do
      Console.WriteLine($"{s.Split(',')}");
      i := i + 1;
    end while;
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string expected;
    for (int k = 0; k < 100; ++k)
        expected += "System.Collections.Generic.Array<string>\n";
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// §28.3.11 / stdlib §9.6 (v0.355) — the `s.ToCharArray()` row:
// `$"{s.ToCharArray()}"`.
//
// The §9.6 sibling of `Split` (`public method ToCharArray(): owned
// Array<char>;`): the same fresh-array obligation, with a value-element
// array (no per-element disposer — the elements are code points).
BEZEL_TEST(interp_hole_residual_8171,
           to_char_array_hole_disposed_at_full_expression_boundary) {
    auto r = runAotChecked(std::string(kPrelude) + R"(
public class Program
  public static method Main(): int
    var s := "a,b,c";
    var i := 0;
    while i < 100 do
      Console.WriteLine($"{s.ToCharArray()}");
      i := i + 1;
    end while;
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string expected;
    for (int k = 0; k < 100; ++k)
        expected += "System.Collections.Generic.Array<char>\n";
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// §28.3 / §28.3.11 (v0.355) — the EXCEPTION-PATH row (issue #8172):
// `$"{C.Make()}{T.Make()}"` where `T.ToString()` THROWS.
//
// The hole's own `ToString` dispatch (§7.4: the hole's implicit dispatch is
// not a member access) is USER CODE, and it may throw. §28.3's
// *Exception-path discharge* requires every already-produced, not-yet-consumed
// unbound owned temporary to be disposed while unwinding — including the
// throwing hole's OWN temporary.
//
// The registration must therefore happen BEFORE the render: #8143's arm ran
// inside `appendPart`, i.e. AFTER the render, so a throwing render skipped
// the registration entirely and the temporary was owned by nobody. MEASURED
// before the fix (100 iterations): the `T` temporary leaked, 1600 B in 100
// allocations; the earlier hole's `C` temporary WAS discharged (100 "D1"
// markers) because that hole's arm had already run.
//
// The accumulator is covered by the same clause: the interpolation's running
// string is an owned temporary of the expression too, and a throw out of the
// render abandoned it (MEASURED before the fix: 3200 B in 100 direct + 400 B
// in 100 indirect allocations per 100 iterations — the accumulator's header
// and its char data; the exception object itself is NOT leaked).
//
// The stdout is asserted EXACTLY: one "DT" (the `T` temporary discharged on
// the unwind) and one "D1" (the `C` temporary discharged on the unwind) per
// iteration, then the catch counter.
BEZEL_TEST(interp_hole_residual_8171,
           render_throws_8172_discharges_the_holes_owned_temporary) {
    auto r = runAotChecked(std::string(kThrowPrelude) + R"(
public class Program
  public static method Main(): int
    var i := 0;
    var caught := 0;
    while i < 100 do
      try
        Console.WriteLine($"{C.Make()}{T.Make()}");
      catch
        InvalidOperationException => do caught := caught + 1; end,
      end try;
      i := i + 1;
    end while;
    Console.WriteLine("caught=" + caught.ToString());
    return 0;
  end Main;
end Program;
)");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string expected;
    for (int k = 0; k < 100; ++k) {
        expected += "DT\n";
        expected += "D1\n";
    }
    expected += "caught=100\n";
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}
