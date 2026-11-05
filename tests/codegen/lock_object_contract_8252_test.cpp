// Issue #8252 — `ToString()` on a `Lock` (stdlib §6.2) was REJECTED by
// codegen's §8.1 universal-Object gate, although the SAME value reached
// through an `Object` carrier rendered "System.Threading.Lock".
//
// THE DEFECT (measured on this lane's base, r30-c1 @ fb1184c7, spec
// revision v0.355):
//
//   lk3.bzl:7:23: member `ToString` on `Lock` is declared via the universal
//   Object contract (language spec §8.1) but is not yet implemented by this
//   compiler for this receiver (tracked #2457)
//
// while the carrier spelling of the very same value
//
//   var o: Object := l; o.ToString()   ->  "System.Threading.Lock"
//
// compiled and printed.  The two spellings of ONE value disagreed.
//
// WHAT THE SPEC REQUIRES.  Language spec §8.1 (v0.355) declares `Object`
// with `ToString(): string` and states that "`Object` is the universal root.
// Every type derives from it"; stdlib §6.2 declares `public class Lock`
// with no public constructor and NO declared instance member other than its
// destructor, so the §8.1 member it inherits has NO override on it.  §8.5
// then fixes the default: "For classes: the type's qualified name" — for the
// handle `Acquire()` returns that is "System.Threading.Lock": the type is
// declared in stdlib §6.2, and the qualification is stdlib §2.13's
// module-path rendering (the type lives in `System.Threading` — see
// `kBuiltinClassModulePaths` in src/common/BuiltinRegistry.cpp).  §8.3 fixes
// the `Equals` / `GetHashCode` default: reference identity / identity hash.
//
// THE MECHANISM.  `Lock`'s TypeRef is a DECLARATION-LESS Named built-in (the
// handle `Mutex.Acquire()` returns carries no TypeSymbol), so codegen's
// §8.1 gate — the one that rejects an §8.1 member on a declaration-less
// built-in receiver — was reached and fired.  The `Object`-carrier spelling
// took the OTHER road: an `Object`-typed receiver dispatches through
// `lowerObjectToString` (vtable slot 3), and the runtime's `bz_lock_vtable`
// slot 3 is `bz_lock_obj_tostring` — the qualified name, installed by #8000
// — so the carrier answered.  The fix gives the DIRECT spelling the same
// answer, through the §8.5 qualified-name literal the house already uses for
// the other built-in classes with no declared override (`Array`, `Stack`,
// `DrainedEntry`, `Stopwatch`, `CancellationToken`): `Lock` joins
// `kNoOverrideIdentityBuiltins` (src/common/BuiltinRegistry.cpp), the
// registry row that selects exactly that dispatch.
//
// ORACLE: exact stdout (runAotChecked).  "<did-not-run>" is the pre-fix
// answer for every rejected row — the program did not compile at all.
//
// Cite: language spec §8.1 / §8.3 / §8.5 (v0.355), stdlib §6.2 (the
// handle type), stdlib §2.13 (the qualified name), language spec §12.7 /
// §19.3 (the runtime type tag, #8000).

#include "support/aot_support.hpp"  // runAotChecked
#include "test_harness.hpp"

#include <string>

using bezel::test::runAotChecked;

namespace {

// stdlib §6.2's `Mutex` / `Lock` are in `System.Threading`, which is NOT
// auto-imported (§2.3), so the import is part of every program here.
std::string prog(const std::string& body) {
    return "module P; import System.Threading; public class Program "
           "public static method Main(): int " + body +
           " return 0; end Main; end Program;";
}

}  // namespace

// ---- the issue's own repro ------------------------------------------------
//
// stdlib §6.2's `Acquire(): owned Lock` handle, rendered by the §8.5
// default.  Pre-fix: a compile error (the §8.1 gate), so the program never
// ran and there was no stdout at all.
BEZEL_TEST(lock_object_contract_8252, lock_tostring_repro_8252) {
    auto r = runAotChecked(prog(
        "  var m := new Mutex(); "
        "  var l := m.Acquire(); "
        "  Console.WriteLine(l.ToString()); "
        "  l.Dispose(); "
        "  m.Dispose(); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "System.Threading.Lock\n");
}

// ---- control: the `Object` carrier keeps working --------------------------
//
// The carrier spelling is the one that WORKED before the fix; it must keep
// working after it, and it must keep answering the SAME string as the
// direct spelling (the issue's "the carrier path and the direct path
// disagree").
BEZEL_TEST(lock_object_contract_8252, object_carrier_to_string_still_works_8252) {
    auto r = runAotChecked(prog(
        "  var m := new Mutex(); "
        "  var l := m.Acquire(); "
        "  var o: Object := l; "
        "  Console.WriteLine(o.ToString()); "
        "  l.Dispose(); "
        "  m.Dispose(); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "System.Threading.Lock\n");
}

// ---- the issue's NARROWED form --------------------------------------------
//
// "The narrowed form fails the same way: `if o is Lock then (o as Lock)!
// .ToString() ...`."  The narrowed receiver is the SAME built-in reached
// through §8.2's runtime type test, so it must render the same string.
BEZEL_TEST(lock_object_contract_8252, narrowed_is_lock_form_8252) {
    auto r = runAotChecked(prog(
        "  var m := new Mutex(); "
        "  var l := m.Acquire(); "
        "  var o: Object := l; "
        "  if o is Lock then "
        "    Console.WriteLine(\"is-Lock: true\"); "
        "    var q := o as Lock; "
        "    Console.WriteLine(q!.ToString()); "
        "  else "
        "    Console.WriteLine(\"is-Lock: false\"); "
        "  end; "
        "  l.Dispose(); "
        "  m.Dispose(); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "is-Lock: true\nSystem.Threading.Lock\n");
}

// ---- the rest of the §8.1 surface on the same receiver --------------------
//
// §8.1 declares `Equals(other: Object?): bool` and `GetHashCode(): int`
// beside `ToString()`, and §8.3 fixes their class default (reference
// identity / identity hash).  The `Object`-carrier spelling of the same
// value answered 1 / True before the fix; the direct spelling was rejected
// with the same §8.1 gate.  The two must now agree — and the row READS the
// values the fix produces rather than comparing against a constant.
BEZEL_TEST(lock_object_contract_8252, lock_equals_and_gethashcode_match_the_carrier_8252) {
    auto r = runAotChecked(prog(
        "  var m := new Mutex(); "
        "  var l := m.Acquire(); "
        "  var o: Object := l; "
        "  Console.WriteLine(l.GetHashCode()); "
        "  Console.WriteLine(o.GetHashCode()); "
        "  Console.WriteLine(l.Equals(l)); "
        "  l.Dispose(); "
        "  m.Dispose(); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n1\nTrue\n");
}

// ---- the receiver OWNS something: loop it ---------------------------------
//
// A `Lock` holds a reference on the mutex's core (`bz_lock_t.core`, §28.13's
// shared core), so a leaked or double-released handle shows up only under
// repetition.  100 acquire / render / release cycles under the checked
// oracle (ASan + LSan + dispose-exactly-once): the row READS the rendered
// string (it is not a constant body), so a fix that rendered the wrong
// string would fail it, and a leak or a double free would fail the oracle.
//
// (Lane-contract item 8: "a leak repro that runs once can hide under the
// sanitizer's noise, so loop it 50-500x".)
BEZEL_TEST(lock_object_contract_8252, lock_tostring_loop_100_owned_core_8252) {
    auto r = runAotChecked(prog(
        "  var m := new Mutex(); "
        "  var n := 0; "
        "  var i := 0; "
        "  while i < 100 do "
        "    using var l := m.Acquire(); "
        "    if l.ToString() = \"System.Threading.Lock\" then "
        "      n := n + 1; "
        "    end; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(n); "
        "  m.Dispose(); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}
