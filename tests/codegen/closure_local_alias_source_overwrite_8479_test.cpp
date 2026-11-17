// Codegen — issue #8479: a closure local bound from ANOTHER function value
// (`var cmp := g;`) took no reference of its own, so overwriting its SOURCE
// freed the function value the alias still called.
//
// THE RULE (§28.9, v0.357, "Runtime-managed types"): a function-typed value is
// runtime-managed — "the type's lifetime is managed by the runtime without
// user involvement". A program that binds a function value to a second name
// has no way to free it early, so the second name must stay callable for as
// long as it is in scope, whatever happens to the first. §27.5 (v0.357) adds,
// for the rows whose lambda captures a variable, that the capture is shared
// with the closure for as long as the closure lives; §21.1 covers the `const`
// spelling of the alias. (§28.9 leaves the lifecycle MECHANISM — reference
// counting — to the implementation; the citation is for the observable
// guarantee, not for the retain/release protocol.)
//
// ROOT CAUSE. `lowerLocalDecl` stored a borrow-initialized closure local with
// NO retain: `var cmp := g;` copied `g`'s pointer and owned nothing. Every
// site that can end the value's life through the OTHER name releases a
// reference its own holder owns — `lowerClosureLocalReassign` when the source
// is reassigned, the field store when the source is a field, the source's own
// scope-end release — and the alias went on calling freed memory. Fix #8440
// gave the alias its own reference only when the body assigns to the ALIAS.
//
// THE FIX. EVERY closure local whose initializer is a borrowed read of a
// function value (a local, a parameter, a field, a built-in collection
// element, an if-expression over those; `var` and `const` alike) takes its
// own reference at the declaration (`closureAliasOwned_`) and is classified
// by `collectClosureReleaseEligible` like a producer-initialized local:
//   * confined to the frame — released at scope end;
//   * leaves the frame (`return cmp;`, a field or collection store, a bind
//     into a non-owning local) — the alias's reference is still released at
//     scope end (`closureEscapingScopeEndRelease_`), without the
//     capture-cell free a confined local gets: that release only undoes the
//     alias's own retain. Before, a returned reassigned alias minted on top
//     of a reference nothing released (#8483). A `return` and a class-field
//     store take a reference of their own; a COLLECTION store does not, and
//     the fix-forward rows at the end of this file are about exactly that;
//   * captured by a lambda — the capture cell takes the alias's reference
//     (issue #8478, its own file).
// A local such an alias was bound from, when the alias is not confined, is
// taken out of the "its closure dies with the scope" set as well: its
// reference is still released at scope end, but the capture cells of its
// lambda stay with the closure (the `alias_of_capturing_source_*` rows —
// freeing them was a use-after-free the unowned alias had been masking with
// a leak).
//
// Every program loops 200 times (a one-shot repro is a vacuous oracle for a
// refcount defect) and runs under generated-code AddressSanitizer AND
// LeakSanitizer (`runAotChecked`), with the full stdout compared against an
// expectation computed here, in C++, from the program's arithmetic.
//
// FAIL-WITHOUT, MEASURED on the pre-fix compiler (r30-c1 at 0cb32bb14,
// `bezelc --sanitize=address --link=system`, ASAN_OPTIONS=detect_leaks=1):
//
//   row                                                               pre-fix                                       with the fix
//   issue_repro_local_source_reassigned_alias_stays_live_spec_28_9    heap-use-after-free in main, 0 lines          clean, 201 lines
//   issue_repro_field_source_stored_over_alias_stays_live_spec_28_9   heap-use-after-free in main, 0 lines          clean, 201 lines
//   issue_leak_face_never_reassigned_alias_releases_source_spec_28_9  leak 64 B, 201 lines                          clean, 201 lines
//   const_alias_source_reassigned_spec_21_1                           heap-use-after-free in main, 0 lines          clean, 201 lines
//   alias_of_alias_source_reassigned_spec_28_9                        heap-use-after-free in main, 0 lines          clean, 201 lines
//   source_outlives_loop_alias_per_iteration_spec_28_9                heap-use-after-free in main, 0 lines          clean, 201 lines
//   alias_scope_left_by_break_and_continue_spec_28_9                  heap-use-after-free in main, 0 lines          clean, 74 lines
//   alias_of_capturing_source_returned_spec_27_5                      leak 15200 B, 401 lines                       clean, 401 lines
//   alias_chain_of_capturing_source_last_returned_spec_27_5           leak 15200 B, 401 lines                       clean, 401 lines
//   alias_of_capturing_source_stored_into_field_spec_27_5             leak 15200 B, 401 lines                       clean, 401 lines
//   alias_of_capturing_source_returned_on_one_path_spec_27_5          leak 15200 B, 301 lines                       clean, 301 lines
//   capturing_source_reassigned_alias_returned_spec_27_5              heap-use-after-free in bz$M.C$Mk, 0 lines     clean, 401 lines
//   lambda_body_alias_of_capturing_source_returned_spec_27_5          leak 15200 B, 401 lines                       clean, 401 lines
//   alias_of_capturing_source_stored_then_unwound_spec_27_5           leak 15200 B, 401 lines                       clean, 401 lines
//   var_source_copied_in_nested_block_spec_27_5                       leak 15200 B, 201 lines                       clean, 201 lines
//   const_source_copied_in_nested_block_spec_21_1                     leak 15200 B, 201 lines                       clean, 201 lines
//   using_var_source_copied_in_nested_block_spec_28_9                 leak 15200 B, 201 lines                       clean, 201 lines
//   control_confined_alias_of_capturing_source_spec_27_5              leak 15200 B, 601 lines                       clean, 601 lines
//
// ("N lines" is the stdout line count before the process ended. The
// `control_` row is a leak pre-fix, not a memory error: the unowned alias
// marked its source escaped and nothing released it.)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {
// The program's stdout when it compiled, ran to exit code 0 and no sanitizer
// (ASan or LSan) fired; a marker otherwise, so a crash can never compare equal
// to an expectation.
std::string runCheckedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}
void line(std::string& e, int v) { e += std::to_string(v) + "\n"; }
}  // namespace

// THE ISSUE'S FIRST REPRO, verbatim (200 iterations). `cmp` aliases `g`; `g` is
// reassigned on the even iterations; `cmp` is called afterwards and must still be
// the original `x - y` (-1). even: -1 + (1 - 4) = -4; odd: -1 + (4 - 1) = 2.
// Pre-fix: the reassignment released the only reference and `cmp(1, 2)` read the
// freed cell.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           issue_repro_local_source_reassigned_alias_stays_live_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, i % 2 == 0 ? -4 : 2);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    for var i in 0..<200 do
      var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
      var cmp: (int, int) -> int := g;
      if i mod 2 = 0 then
        g := lambda(x: int, y: int) => y - x;
      end if;
      Console.WriteLine(cmp(1, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// THE ISSUE'S SECOND REPRO, verbatim: the source is a FIELD and is stored over.
// i = 0: `cmp` is the field initializer `x - y` (-1), the new `h.F` gives -3.
// i > 0: `cmp` is the previous iteration's `y - x` (1), plus -3.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           issue_repro_field_source_stored_over_alias_stays_live_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, i == 0 ? -4 : -2);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class H
  public F: (int, int) -> int := lambda(x: int, y: int) => x - y;
  public constructor() end constructor;
end H;
public class C
  public static method Main(): int
    using var h := new H();
    for var i in 0..<200 do
      var cmp: (int, int) -> int := h.F;
      h.F := lambda(x: int, y: int) => y - x;
      Console.WriteLine(cmp(1, 2) + h.F(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// THE ISSUE'S LEAK FACE, verbatim: nothing is reassigned. Pre-fix the unowned
// alias marked its source "escaped", so `g`'s own scope-end release was never
// armed and LeakSanitizer reported the cell. -1 + 3 = 2.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           issue_leak_face_never_reassigned_alias_releases_source_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 2);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<200 do
      var cmp: (int, int) -> int := g;
      Console.WriteLine(cmp(1, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The §21.1 `const` spelling of the alias (a second declaration node kind:
// LocalConstDecl). Same arithmetic as the first repro.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           const_alias_source_reassigned_spec_21_1) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, i % 2 == 0 ? -4 : 2);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    for var i in 0..<200 do
      var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
      const cmp: (int, int) -> int := g;
      if i mod 2 = 0 then
        g := lambda(x: int, y: int) => y - x;
      end if;
      Console.WriteLine(cmp(1, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// A chain `a := g; b := a;`: three names, each with its own reference. a and b
// stay `x - y` (-1 each); g is -3 on even iterations, 3 on odd ones.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_of_alias_source_reassigned_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, i % 2 == 0 ? -5 : 1);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    for var i in 0..<200 do
      var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
      var a: (int, int) -> int := g;
      var b: (int, int) -> int := a;
      if i mod 2 = 0 then
        g := lambda(x: int, y: int) => y - x;
      end if;
      Console.WriteLine(a(1, 2) + b(1, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The source is declared OUTSIDE the loop and reassigned on every iteration; the
// alias is re-bound per iteration and must hold the value the source had at the
// bind. i = 0: -1 + -3; later: 1 + -3.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           source_outlives_loop_alias_per_iteration_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, i == 0 ? -4 : -2);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<200 do
      var cmp: (int, int) -> int := g;
      g := lambda(x: int, y: int) => y - x;
      Console.WriteLine(cmp(1, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// `continue` (i mod 4 = 1, before the source is overwritten) and `break`
// (i = 98, after) leave the alias's scope too: its reference is released on
// each. 73 lines.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_scope_left_by_break_and_continue_spec_28_9) {
    std::string e;
    for (int i = 0; i < 98; ++i) { if (i % 4 == 1) continue; line(e, i == 0 ? -4 : -2); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
    for var i in 0..<200 do
      var cmp: (int, int) -> int := g;
      if i mod 4 = 1 then
        continue;
      end if;
      g := lambda(x: int, y: int) => y - x;
      if i = 98 then
        break;
      end if;
      Console.WriteLine(cmp(1, 2) + g(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// §27.5: the source's lambda CAPTURES a local, and the alias — not the
// source — is what leaves the method. The closure outlives the source's scope
// through the alias, so the capture must stay readable: the source may release
// its reference at scope end, but the capture cell stays with the closure.
// Mk prints cmp(3, 1) + g(1, 1) = (2 + k) + k; the caller prints
// h(1, 2) + h(4, 1) = (k - 1) + (k + 3).
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_of_capturing_source_returned_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) { line(e, 2 + 2 * i); line(e, 2 + 2 * i); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Mk(k: int): (int, int) -> int
    var kk: int := k;
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + kk;
    var cmp: (int, int) -> int := g;
    Console.WriteLine(cmp(3, 1) + g(1, 1));
    return cmp;
  end Mk;
  public static method Main(): int
    for var i in 0..<200 do
      var h := C.Mk(i);
      Console.WriteLine(h(1, 2) + h(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The same through a chain `a := g; b := a; return b;`: every link the value
// passes through on its way out must keep the capture alive.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_chain_of_capturing_source_last_returned_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) { line(e, 2 * i); line(e, 2 + 2 * i); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Mk(k: int): (int, int) -> int
    var kk: int := k;
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + kk;
    var a: (int, int) -> int := g;
    var b: (int, int) -> int := a;
    Console.WriteLine(g(1, 1) + a(1, 1));
    return b;
  end Mk;
  public static method Main(): int
    for var i in 0..<200 do
      var h := C.Mk(i);
      Console.WriteLine(h(1, 2) + h(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The alias of a capturing source leaves through a FIELD store. Set prints
// g(1, 1) = k; the caller reads the field after Set returned.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_of_capturing_source_stored_into_field_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) { line(e, i); line(e, 2 + 2 * i); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class H
  public F: (int, int) -> int := lambda(x: int, y: int) => x - y;
  public constructor() end constructor;
end H;
public class C
  public static method Set(h: H, k: int)
    var kk: int := k;
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + kk;
    var cmp: (int, int) -> int := g;
    h.F := cmp;
    Console.WriteLine(g(1, 1));
  end Set;
  public static method Main(): int
    using var h := new H();
    for var i in 0..<200 do
      C.Set(h, i);
      Console.WriteLine(h.F(1, 2) + h.F(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The alias is returned on one path only; on the other both names die at scope
// end and a fresh lambda is returned. even: the returned alias, 2 + 2k. odd:
// cmp(3, 1) + g(1, 1) = 2 + 2k inside Mk, then `y - x` at (1, 2) and (4, 1): -2.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_of_capturing_source_returned_on_one_path_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) { line(e, 2 + 2 * i); if (i % 2 != 0) line(e, -2); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Mk(k: int, c: bool): (int, int) -> int
    var kk: int := k;
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + kk;
    var cmp: (int, int) -> int := g;
    if c then
      return cmp;
    end if;
    Console.WriteLine(cmp(3, 1) + g(1, 1));
    return lambda(x: int, y: int) => y - x;
  end Mk;
  public static method Main(): int
    for var i in 0..<200 do
      var h := C.Mk(i, i mod 2 = 0);
      Console.WriteLine(h(1, 2) + h(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Both at once: the capturing source is REASSIGNED (even iterations) and the
// alias is returned. cmp(3, 1) = 2 + k either way, g(1, 1) = k either way.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           capturing_source_reassigned_alias_returned_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) { line(e, 2 + 2 * i); line(e, 2 + 2 * i); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Mk(k: int, c: bool): (int, int) -> int
    var kk: int := k;
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + kk;
    var cmp: (int, int) -> int := g;
    if c then
      g := lambda(x: int, y: int) => y - x + kk;
    end if;
    Console.WriteLine(cmp(3, 1) + g(1, 1));
    return cmp;
  end Mk;
  public static method Main(): int
    for var i in 0..<200 do
      var h := C.Mk(i, i mod 2 = 0);
      Console.WriteLine(h(1, 2) + h(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The second venue: the source and its alias are locals of a LAMBDA body.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           lambda_body_alias_of_capturing_source_returned_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) { line(e, 2 + 2 * i); line(e, 2 + 2 * i); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    for var i in 0..<200 do
      var mk := lambda(k: int): (int, int) -> int => do
        var kk: int := k;
        var g: (int, int) -> int := lambda(x: int, y: int) => x - y + kk;
        var cmp: (int, int) -> int := g;
        Console.WriteLine(cmp(3, 1) + g(1, 1));
        return cmp;
      end;
      var h := mk(i);
      Console.WriteLine(h(1, 2) + h(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The frame is left by an EXCEPTION after the alias was stored into a field
// (even iterations): the unwind edge releases the two names' references and
// the field's closure stays callable.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_of_capturing_source_stored_then_unwound_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) { line(e, i % 2 == 0 ? -1000 : 2 + 2 * i); line(e, 2 + 2 * i); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class H
  public F: (int, int) -> int := lambda(x: int, y: int) => x - y;
  public constructor() end constructor;
end H;
public class C
  public static method Put(h: H, k: int, c: bool)
    var kk: int := k;
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + kk;
    var cmp: (int, int) -> int := g;
    h.F := cmp;
    if c then
      throw new InvalidOperationException("x");
    end if;
    Console.WriteLine(cmp(3, 1) + g(1, 1));
  end Put;
  public static method Main(): int
    using var h := new H();
    for var i in 0..<200 do
      try
        C.Put(h, i, i mod 2 = 0);
      catch
        InvalidOperationException => Console.WriteLine(-1000),
      end try;
      Console.WriteLine(h.F(1, 2) + h.F(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// A capturing closure copied into a second local in a NESTED block (the shape
// of #7288), `var` source. Pre-fix the copy marked the source escaped: neither
// the closure nor its capture cell was ever released (76 B per iteration).
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           var_source_copied_in_nested_block_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 2 * i);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    for var i in 0..<200 do
      var k: int := i;
      var c := lambda(): int => k;
      do
        var d := c;
        Console.WriteLine(d() + c());
      end;
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The same with a §21.1 `const` source.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           const_source_copied_in_nested_block_spec_21_1) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 2 * i);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    for var i in 0..<200 do
      var k: int := i;
      const c := lambda(): int => k;
      do
        var d := c;
        Console.WriteLine(d() + c());
      end;
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The same with a `using var` source (§28.9: "a plain `var` of function type
// scopes its captures identically").
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           using_var_source_copied_in_nested_block_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 2 * i);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    for var i in 0..<200 do
      var k: int := i;
      using var c := lambda(): int => k;
      do
        var d := c;
        Console.WriteLine(d() + c());
      end;
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// CONTROL: neither name leaves the frame. Both references and the capture cell
// are released at scope end (no leak). cmp(3, 1) + g(1, j) with k = 2i.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           control_confined_alias_of_capturing_source_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) for (int j = 0; j < 3; ++j) line(e, 3 - j + 4 * i);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runCheckedOut(R"BZL(
module M;
public class C
  public static method Main(): int
    for var i in 0..<200 do
      var k := i * 2;
      var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
      var cmp: (int, int) -> int := g;
      for var j in 0..<3 do
        Console.WriteLine(cmp(3, 1) + g(1, j));
      end for;
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// ===========================================================================
// FIX-FORWARD (review REJECT 1 of the fix above).
//
// THE REGRESSION. The first form of the fix released an owned alias at scope
// end AND kept releasing the alias's SOURCE, on the premise that every site
// through which the alias's value leaves the frame takes a reference of its
// own. A collection store takes none: `Array<F>.Add`, `Insert`, the indexer
// store and a `Dictionary` value store keep the pointer and own nothing
// (#5216, #5866). So `var g := lambda …; var a := g; fs.Add(a);`, with `fs`
// outliving the block or the call, released both references and left `fs`
// holding a freed function value: programs that merely LEAKED before the fix
// became heap-use-after-free (§28.9, v0.357: the value's lifetime is the
// runtime's business, so a stored function value must stay callable).
//
// THE FIX-FORWARD. Binding a source into an owned alias is a non-consuming
// read of the source only while the alias never hands the value to a site
// that keeps it without a reference. For an alias that does, the bind is an
// escape of the source, as it was before the fix: the source keeps "escaped,
// not released", exactly the verdict of the alias-free `fs.Add(g)`. The alias
// itself still retains and releases (net zero).
//
// THE ORACLE. These programs still LEAK the stored function value (so does
// `fs.Add(g)` with no alias, on every tree: the borrowing collection store is
// #5866's). The rows therefore run under generated-code AddressSanitizer with
// LeakSanitizer off (`runAotCheckedLenient`) and compare the full stdout: they
// assert "no memory error and the right output", not "no leak".
//
// MEASURED, `bezelc --sanitize=address --link=system`,
// ASAN_OPTIONS=detect_leaks=1, 200 iterations ("N lines" = stdout lines
// before the process ended; "UAF" = AddressSanitizer heap-use-after-free):
//
//   row                                                           r30-c1 (before #8479)   first #8479 fix   with the fix-forward
//   alias_added_to_array_outliving_block_spec_28_9                leak, 201 lines         UAF, 0 lines      leak, 201 lines
//   alias_added_to_callers_array_spec_27_5                        leak, 201 lines         UAF, 0 lines      leak, 201 lines
//   const_alias_added_to_array_outliving_block_spec_21_1          leak, 201 lines         UAF, 0 lines      leak, 201 lines
//   alias_added_to_array_held_by_object_spec_27_5                 leak, 201 lines         UAF, 0 lines      leak, 201 lines
//   alias_stored_through_array_indexer_spec_28_9                  leak, 201 lines         UAF, 0 lines      leak, 201 lines
//   alias_of_alias_added_to_callers_array_spec_27_5               leak, 201 lines         UAF, 0 lines      leak, 201 lines
//   alias_inserted_into_callers_array_spec_27_5                   leak, 201 lines         UAF, 0 lines      leak, 201 lines
//   alias_stored_as_dictionary_value_spec_27_5                    leak, 201 lines         UAF, 0 lines      leak, 201 lines
//   alias_of_call_result_added_to_callers_array_spec_27_5         leak, 201 lines         UAF, 0 lines      leak, 201 lines
//   alias_of_method_reference_added_to_callers_array_spec_28_9    leak, 201 lines         UAF, 0 lines      leak, 201 lines
//   alias_assigned_onward_and_added_to_callers_array_spec_27_5    leak, 401 lines         UAF, 1 line       leak, 401 lines
//   alias_in_returned_collection_literal_spec_27_5                leak, 201 lines         UAF, 0 lines      leak, 201 lines
//   alias_added_inside_lambda_body_spec_27_5                      leak, 201 lines         UAF, 0 lines      leak, 201 lines
//   alias_passed_to_method_that_stores_it_spec_27_5               leak, 201 lines         UAF, 0 lines      leak, 201 lines
//   alias_bound_to_nullable_local_then_added_spec_27_5            leak, 201 lines         UAF, 0 lines      leak, 201 lines
//   alias_stored_into_returned_record_spec_27_5                   leak, 201 lines         UAF, 0 lines      leak, 201 lines
//   alias_case_binding_added_to_callers_array_spec_27_5           leak, 201 lines         UAF, 0 lines      leak, 201 lines
//
// Every row is RED on the first #8479 fix (this commit's parent) and green
// with the fix-forward; none is a control.
// ===========================================================================
namespace {
// The program's stdout when it compiled, ran to exit code 0 and no memory
// error fired (AddressSanitizer on, LeakSanitizer off — see THE ORACLE above);
// a marker otherwise.
std::string runNoMemoryErrorOut(const std::string& prog) {
    auto r = runAotCheckedLenient(prog);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}
}  // namespace

// Reviewer's a01. The alias is added to an array that OUTLIVES the block both
// names are declared in; the array is called after the block.
// fs[0](1, 2) + fs[0](4, 1) = -1 + 3 = 2.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_added_to_array_outliving_block_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 2);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(
module M;
public class C
  public static method Main(): int
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
      do
        var g: (int, int) -> int := lambda(x: int, y: int) => x - y;
        var a: (int, int) -> int := g;
        fs.Add(a);
      end;
      Console.WriteLine(fs[0](1, 2) + fs[0](4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Reviewer's a02. A helper adds the alias to the CALLER's array; the lambda
// captures the helper's parameter. (1 - 2 + i) + (4 - 1 + i) = 2 + 2i.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_added_to_callers_array_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 2 + 2 * i);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(
module M;
public class C
  public static method Fill(fs: Array<(int, int) -> int>, k: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var a: (int, int) -> int := g;
    fs.Add(a);
  end Fill;
  public static method Main(): int
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
      Fill(fs, i);
      Console.WriteLine(fs[0](1, 2) + fs[0](4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Reviewer's a19. The `const` spelling of the alias (§21.1), added to an
// array that outlives the block. 1 - 2 + i = i - 1.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           const_alias_added_to_array_outliving_block_spec_21_1) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, i - 1);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(
module M;
public class C
  public static method Main(): int
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
      do
        var g: (int, int) -> int := lambda(x: int, y: int) => x - y + i;
        const a: (int, int) -> int := g;
        fs.Add(a);
      end;
      Console.WriteLine(fs[0](1, 2));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Reviewer's b05. The array is an `owned` field of an object the caller
// holds; the helper adds the alias through it. h.Fs[i](1, 2) = i - 1.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_added_to_array_held_by_object_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, i - 1);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(
module M;
public class H
  public owned Fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
  public constructor() end constructor;
end H;
public class C
  public static method Install(h: H, k: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var a: (int, int) -> int := g;
    h.Fs.Add(a);
  end Install;
  public static method Main(): int
    using var h := new H();
    for var i in 0..<200 do
      Install(h, i);
      Console.WriteLine(h.Fs[i](1, 2));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Reviewer's b07. The alias is stored through the array INDEXER (`fs[0] := a`)
// rather than `Add`. 1 - 2 + i = i - 1.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_stored_through_array_indexer_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, i - 1);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(
module M;
public class C
  public static method Main(): int
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
      fs.Add(lambda(x: int, y: int) => 0);
      do
        var g: (int, int) -> int := lambda(x: int, y: int) => x - y + i;
        var a: (int, int) -> int := g;
        fs[0] := a;
      end;
      Console.WriteLine(fs[0](1, 2));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The same defect through a CHAIN: `b` is the alias that is added, `a` is
// where its value came from, and `g` is what both keep alive. Pins that
// "kept without a reference" spreads from `b` to `a` (a fix that looked only
// at the alias bound DIRECTLY from the source would still release `g`).
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_of_alias_added_to_callers_array_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 2 + 2 * i);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(
module M;
public class C
  public static method Fill(fs: Array<(int, int) -> int>, k: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var a: (int, int) -> int := g;
    var b: (int, int) -> int := a;
    fs.Add(b);
  end Fill;
  public static method Main(): int
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
      Fill(fs, i);
      Console.WriteLine(fs[0](1, 2) + fs[0](4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// `Insert` instead of `Add`.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_inserted_into_callers_array_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 2 + 2 * i);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(
module M;
public class C
  public static method Fill(fs: Array<(int, int) -> int>, k: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var a: (int, int) -> int := g;
    fs.Insert(0, a);
  end Fill;
  public static method Main(): int
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
      Fill(fs, i);
      Console.WriteLine(fs[0](1, 2) + fs[0](4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// A `Dictionary` value store (`d[k] := a`).
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_stored_as_dictionary_value_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 2 + 2 * i);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(
module M;
public class C
  public static method Fill(d: Dictionary<int, (int, int) -> int>, k: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var a: (int, int) -> int := g;
    d[k] := a;
  end Fill;
  public static method Main(): int
    for var i in 0..<200 do
      var d: Dictionary<int, (int, int) -> int> := new Dictionary<int, (int, int) -> int>();
      Fill(d, i);
      Console.WriteLine(d[i](1, 2) + d[i](4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The source is a local initialized from a CALL RESULT, not from a lambda
// literal.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_of_call_result_added_to_callers_array_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 2 + 2 * i);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(
module M;
public class C
  public static method Mk(k: int): (int, int) -> int
    return lambda(x: int, y: int) => x - y + k;
  end Mk;
  public static method Fill(fs: Array<(int, int) -> int>, k: int)
    var g: (int, int) -> int := Mk(k);
    var a: (int, int) -> int := g;
    fs.Add(a);
  end Fill;
  public static method Main(): int
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
      Fill(fs, i);
      Console.WriteLine(fs[0](1, 2) + fs[0](4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The source is a local initialized from a METHOD REFERENCE.
// (1 - 2) + (4 - 1) + i = 2 + i.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_of_method_reference_added_to_callers_array_spec_28_9) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 2 + i);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(
module M;
public class C
  public static method Sub(x: int, y: int): int
    return x - y;
  end Sub;
  public static method Fill(fs: Array<(int, int) -> int>)
    var g: (int, int) -> int := Sub;
    var a: (int, int) -> int := g;
    fs.Add(a);
  end Fill;
  public static method Main(): int
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
      Fill(fs);
      Console.WriteLine(fs[0](1, 2) + fs[0](4, 1) + i);
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The alias is ALSO assigned into another closure local (#4473's arm, which
// by itself lets the alias be released at scope end). The collection store
// still decides the source. o(5, 1) = 4 + k, then 2 + 2k from the array.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_assigned_onward_and_added_to_callers_array_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) { line(e, 4 + i); line(e, 2 + 2 * i); }
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(
module M;
public class C
  public static method Fill(fs: Array<(int, int) -> int>, k: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var a: (int, int) -> int := g;
    var o: (int, int) -> int := lambda(x: int, y: int) => 0;
    o := a;
    fs.Add(a);
    Console.WriteLine(o(5, 1));
  end Fill;
  public static method Main(): int
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
      Fill(fs, i);
      Console.WriteLine(fs[0](1, 2) + fs[0](4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The alias is an element of a COLLECTION LITERAL the method returns.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_in_returned_collection_literal_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 2 + 2 * i);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(
module M;
public class C
  public static method Make(k: int): owned Array<(int, int) -> int>
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var a: (int, int) -> int := g;
    var fs: Array<(int, int) -> int> := [a];
    return fs;
  end Make;
  public static method Main(): int
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := Make(i);
      Console.WriteLine(fs[0](1, 2) + fs[0](4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// Both names live in a LAMBDA BODY; the array is a captured local of the
// enclosing method.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_added_inside_lambda_body_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 2 + 2 * i);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(
module M;
public class C
  public static method Main(): int
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
      var fill: (int) -> int := lambda(k: int) => do
        var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
        var a: (int, int) -> int := g;
        fs.Add(a);
        return 0;
      end;
      fill(i);
      Console.WriteLine(fs[0](1, 2) + fs[0](4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The alias is an ARGUMENT, and the callee is what stores it (a generic
// container's `Put(x: T)` adding to its own array).
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_passed_to_method_that_stores_it_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 2 + 2 * i);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(
module M;
public class Bag<T>
  public owned Items: Array<T> := new Array<T>();
  public constructor() end constructor;
  public method Put(x: T)
    Items.Add(x);
  end Put;
end Bag;
public class C
  public static method Fill(b: Bag<(int, int) -> int>, k: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var a: (int, int) -> int := g;
    b.Put(a);
  end Fill;
  public static method Main(): int
    using var b := new Bag<(int, int) -> int>();
    for var i in 0..<200 do
      Fill(b, i);
      Console.WriteLine(b.Items[i](1, 2) + b.Items[i](4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The alias is bound into a NULLABLE function-typed local (which takes no
// reference of its own), and that local is what is added.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_bound_to_nullable_local_then_added_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 2 + 2 * i);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(
module M;
public class C
  public static method Fill(fs: Array<(int, int) -> int>, k: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var a: (int, int) -> int := g;
    var n: ((int, int) -> int)? := a;
    if n <> null then
      fs.Add(n!);
    end if;
  end Fill;
  public static method Main(): int
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
      Fill(fs, i);
      Console.WriteLine(fs[0](1, 2) + fs[0](4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The alias becomes the field of a RECORD the method returns (`new R(a)`; a
// record field store is a plain store, unlike a class instance field's).
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_stored_into_returned_record_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 2 + 2 * i);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(
module M;
public record R(F: (int, int) -> int);
public class C
  public static method Make(k: int): R
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var a: (int, int) -> int := g;
    return new R(a);
  end Make;
  public static method Main(): int
    for var i in 0..<200 do
      var r := Make(i);
      Console.WriteLine(r.F(1, 2) + r.F(4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}

// The alias is a `case` scrutinee and the ARM'S BINDING is what is added.
BEZEL_TEST(closure_local_alias_source_overwrite_8479,
           alias_case_binding_added_to_callers_array_spec_27_5) {
    std::string e;
    for (int i = 0; i < 200; ++i) line(e, 2 + 2 * i);
    e += "DONE\n";
    BEZEL_EXPECT_EQ(runNoMemoryErrorOut(R"BZL(
module M;
public class C
  public static method Fill(fs: Array<(int, int) -> int>, k: int)
    var g: (int, int) -> int := lambda(x: int, y: int) => x - y + k;
    var a: (int, int) -> int := g;
    case a of
      var b => do fs.Add(b); end
    end case;
  end Fill;
  public static method Main(): int
    for var i in 0..<200 do
      var fs: Array<(int, int) -> int> := new Array<(int, int) -> int>();
      Fill(fs, i);
      Console.WriteLine(fs[0](1, 2) + fs[0](4, 1));
    end for;
    Console.WriteLine("DONE");
    return 0;
  end Main;
end C;
)BZL"), e);
}
