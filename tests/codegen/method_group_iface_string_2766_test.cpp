// Regression pin for issue #2766 — a METHOD-GROUP reference to a builtin
// generic interface method (`var f := eq.Equals;` where `eq: IEquatable<string>`).
//
// THE BUG. Emitter::makeMethodThunk (src/codegen/LlvmBackend_Layout.cpp) emits
// the thunk body that a method-group reference is backed by. For an interface
// method it dispatches dynamically: `vptr = load recv[0]`, then the itmap at
// `vptr[1]`, then bz_itable_lookup. stdlib §9.6 (v0.240) declares
//   `public class String: IEnumerable<char>, IComparable<string>, IEquatable<string>`
// so `var eq: IEquatable<string> := s;` is a legal conversion — but spec §4.6
// makes `string` a runtime-managed REFERENCE, so the slot holds a bare
// `bz_string*` whose word 0 is `char* data`, NOT a vtable. The itmap load
// therefore reads `data + 8`, past the end of the payload. On the plain oracle
// every shape below exited 139 (SIGSEGV); under ASan it reports a
// global-/heap-buffer-overflow "in thunk$0".
//
// This is the same defect #2752 fixed in emitMethodCall's interface arm, at the
// sibling site. It was NOT fixed by that commit: thunks are cached
// receiver-independently, so the method-group form kept crashing.
//
// SECOND, INDEPENDENT DEFECT AT THE SAME SITE (found while fixing the first,
// and pinned by heap_string_argument_is_not_truncated below). The thunk took
// its LLVM signature from methodFnType(ms), which lowers the INTERFACE's
// declared parameter types; for `IEquatable<T>.Equals(other: T)` the unbound `T`
// lowers to a placeholder `i32` for EVERY instantiation. The emitted thunk
// therefore advertised `i1 (ptr env, i32)` while the call site and the itable
// target both use `ptr`, and it compiled to `mov %esi,%ebx` / `mov %ebx,%esi` —
// TRUNCATING the reference argument to 32 bits. A string literal in a small
// non-PIE executable lives below 4 GiB so it survived; a HEAP string does not.
//
// THE FIX substitutes the receiver's resolved type ARGUMENT for the two
// document-backed signatures (stdlib §9.4 `CompareTo(other: T)`, §9.6
// `Equals`), which both repairs the ABI and separates the `string`
// instantiation from the `int` one in the thunk cache — a prerequisite, because
// only then is the argument a pointer that can be tested at all.
//
// THE ORACLE. The value cases run on the PLAIN oracle: before the fix the
// program dies of SIGSEGV, so `ran` is false and run() yields -999 — a value
// mismatch, caught with no sanitizer. The ASan cases additionally pin the
// out-of-bounds read itself. Every expected value is < 256, since runAot's
// exitCode is WEXITSTATUS-masked to 8 bits.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

bool wasSkipped(const JitResult& r) {
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.message.find("SKIP") != std::string::npos) return true;
    }
    return false;
}

int runPlain(const std::string& src) {
    JitResult r = bezel::test::runAot(src);
    return r.ran ? r.exitCode : -999;
}

int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return -1000;   // ASan toolchain absent — case skipped
    return r.ran ? r.exitCode : -999;
}

// ASan on, LeakSanitizer off. Used only where the program carries a
// PRE-EXISTING leak unrelated to this fix (boxing a primitive into a builtin
// interface, #2746; a concatenated string temporary, the #2717/#2764 family),
// each confirmed to leak identically against the pre-fix compiler. LSan
// overrides the process exit code, so leaving it on would mask the value.
int runCheckedNoLeaks(const std::string& src) {
    JitResult r = runAotCheckedLenient(src);
    if (wasSkipped(r)) return -1000;
    return r.ran ? r.exitCode : -999;
}

// The reported repro: a string in an IEquatable<string> slot, referenced as a
// method group and then invoked.
constexpr const char* kLocalGroup =
    "module M; "
    "class C public static method Main(): int "
    "  var s := \"mg2766-local\"; "
    "  var eq: IEquatable<string> := s; "
    "  var f := eq.Equals; "
    "  if f(\"mg2766-local\") then return 7; end; "
    "  return 3; "
    "end Main; end C;";

}  // namespace

// §4.6 / stdlib §9.6 — the issue's own repro, on the plain oracle.
BEZEL_TEST(method_group_iface_string_2766, local_method_group_string_equals) {
    BEZEL_EXPECT_EQ(runPlain(kLocalGroup), 7);
}

// The same program under ASan: this is what pins the out-of-bounds READ (8
// bytes at `data + 8`) rather than merely the wrong control flow.
BEZEL_TEST(method_group_iface_string_2766, local_method_group_no_oob_read) {
    BEZEL_EXPECT_EQ(runChecked(kLocalGroup), 7);
}

// §27.6 — the thunk is reached by more than the `var f := ...` spelling. Here
// the method group is passed directly as a function-typed ARGUMENT.
BEZEL_TEST(method_group_iface_string_2766, method_group_as_function_argument) {
    BEZEL_EXPECT_EQ(runPlain(
        "module M; "
        "class C "
        "  public static method Apply(f: (string) -> bool): bool "
        "    return f(\"mg2766-arg\"); end Apply; "
        "  public static method Main(): int "
        "    var s := \"mg2766-arg\"; "
        "    var eq: IEquatable<string> := s; "
        "    if Apply(eq.Equals) then return 8; end; "
        "    return 3; "
        "  end Main; "
        "end C;"), 8);
}

// §27.6 — and by RETURNING the method group as a function value.
BEZEL_TEST(method_group_iface_string_2766, method_group_returned_from_method) {
    BEZEL_EXPECT_EQ(runPlain(
        "module M; "
        "class C "
        "  public static method Get(e: IEquatable<string>): (string) -> bool "
        "    return e.Equals; end Get; "
        "  public static method Main(): int "
        "    var s := \"mg2766-ret\"; "
        "    var eq: IEquatable<string> := s; "
        "    var f := Get(eq); "
        "    if f(\"mg2766-ret\") then return 9; end; "
        "    return 3; "
        "  end Main; "
        "end C;"), 9);
}

// stdlib §9.4 `IComparable<T>.CompareTo(other: T): int` with T = string. §9.6
// gives `CompareTo` ordinal semantics, so equal strings compare 0.
BEZEL_TEST(method_group_iface_string_2766, comparable_string_method_group) {
    BEZEL_EXPECT_EQ(runPlain(
        "module M; "
        "class C public static method Main(): int "
        "  var s := \"mg2766-cmp\"; "
        "  var cp: IComparable<string> := s; "
        "  var f := cp.CompareTo; "
        "  if f(\"mg2766-cmp\") = 0 then return 11; end; "
        "  return 3; "
        "end Main; end C;"), 11);
}

// CONTROL (passes before AND after). A genuine class receiver declaring the
// interface must keep dispatching through the itable to its OWN implementation
// — the guard must not divert it to the string intrinsics. Run under ASan so a
// wrongly-routed class receiver fails on the memory error, not on a value
// coincidence.
BEZEL_TEST(method_group_iface_string_2766, user_class_receiver_still_dispatches) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class SC: IEquatable<string> "
        "  public method Equals(other: string): bool => other = \"mg2766-user\"; "
        "end SC; "
        "class C public static method Main(): int "
        "  using var sc := new SC(); "
        "  var e: IEquatable<string> := sc; "
        "  var f := e.Equals; "
        "  if f(\"mg2766-user\") then return 13; end; "
        "  return 3; "
        "end Main; end C;"), 13);
}

// CONTROL (passes before AND after). A boxed primitive in IComparable<int> is
// the value-shaped instantiation: its thunk argument is a genuine `i32`, so the
// string guard must NOT be emitted for it (calling bz_obj_is_string on an
// integer would dereference it as an address).
BEZEL_TEST(method_group_iface_string_2766, boxed_primitive_receiver_still_dispatches) {
    BEZEL_EXPECT_EQ(runCheckedNoLeaks(
        "module M; "
        "class C public static method Main(): int "
        "  var c: IComparable<int> := 5; "
        "  var f := c.CompareTo; "
        "  if f(3) > 0 then return 17; end; "
        "  return 3; "
        "end Main; end C;"), 17);
}

// The shared-thunk case, which is why the fix has to live INSIDE the thunk body
// and not at its mint site. All three method groups below resolve to the same
// IEquatable<string>.Equals MethodSymbol; before this fix one cached `thunk$0`
// served every one of them. Each receiver must get its own correct answer:
// 1 = user class (its own Equals), 2 = string (§9.6 content equality),
// 4 = a string reached through an Object slot. 1 + 2 + 4 = 7.
//
// #2765 (stdlib §2.8/§2.9, v0.282): the third arm used to spell that Object
// slot as `var e3: IEquatable<Object> := o;`. That conversion is now REJECTED
// — stdlib §2.8/§2.9 records that `string` conforms at `string` only and
// `IEquatable<T>` is invariant (§12.9), so no `IEquatable<Object>` view of a
// string exists. The arm is respelled `var e3: IEquatable<string> := o;`,
// which is the SAME shape this test is about and is strictly closer to the
// property named above: the receiver still arrives through an `Object` slot
// (the §5.1 Object→reference conversion, untouched by #2765), and the method
// group now resolves to the very same `IEquatable<string>.Equals` symbol AND
// the same `string` type argument as `f1`/`f2` — so all three groups share one
// thunk-cache key, which is exactly the collision the #2766 fix had to keep
// correct. VERIFIED to still return 7 against a pre-#2765 compiler, so the
// respelling changes nothing this test was pinning. (The differing-type-
// argument axis the old `IEquatable<Object>` spelling also touched is covered
// on its own by int_and_string_comparable_in_one_program below.)
BEZEL_TEST(method_group_iface_string_2766, one_cached_thunk_three_receivers) {
    BEZEL_EXPECT_EQ(runPlain(
        "module M; "
        "class SC: IEquatable<string> "
        "  public method Equals(other: string): bool => other = \"mg2766-u\"; "
        "end SC; "
        "class C public static method Main(): int "
        "  using var sc := new SC(); "
        "  var e1: IEquatable<string> := sc; "
        "  var f1 := e1.Equals; "
        "  var s := \"mg2766-s\"; "
        "  var e2: IEquatable<string> := s; "
        "  var f2 := e2.Equals; "
        "  var o: Object := s; "
        "  var e3: IEquatable<string> := o; "
        "  var f3 := e3.Equals; "
        "  var n := 0; "
        "  if f1(\"mg2766-u\") then n := n + 1; end; "
        "  if f2(\"mg2766-s\") then n := n + 2; end; "
        "  if f3(\"mg2766-s\") then n := n + 4; end; "
        "  return n; "
        "end Main; end C;"), 7);
}

// The reference-vs-value instantiations of ONE interface method in ONE program.
// They need DIFFERENT thunk ABIs (`ptr` vs `i32`), which is why the thunk cache
// key had to gain the signature. 1 = int CompareTo(3) > 0, 2 = equal strings
// compare 0.
BEZEL_TEST(method_group_iface_string_2766, int_and_string_comparable_in_one_program) {
    BEZEL_EXPECT_EQ(runCheckedNoLeaks(
        "module M; "
        "class C public static method Main(): int "
        "  var ci: IComparable<int> := 5; "
        "  var f1 := ci.CompareTo; "
        "  var s := \"mg2766-both\"; "
        "  var cs: IComparable<string> := s; "
        "  var f2 := cs.CompareTo; "
        "  var n := 0; "
        "  if f1(3) > 0 then n := n + 1; end; "
        "  if f2(\"mg2766-both\") = 0 then n := n + 2; end; "
        "  return n; "
        "end Main; end C;"), 3);
}

// The ABI-truncation defect, isolated. The receiver here is a well-behaved user
// class — nothing to do with a string RECEIVER — and the argument is a string
// built at run time, so it lives on the heap. Before the fix the thunk truncated
// it to 32 bits and the callee dereferenced the truncated address: under ASan
// (whose allocator hands out addresses above 4 GiB) that is a hard SEGV inside
// bz$M.SC$Equals. On the plain oracle this program returns 43 either way,
// because a small program's malloc'd addresses happen to fit in 32 bits — which
// is exactly why the defect went unnoticed, and why the ASan oracle is the one
// that pins it.
BEZEL_TEST(method_group_iface_string_2766, heap_string_argument_is_not_truncated) {
    BEZEL_EXPECT_EQ(runCheckedNoLeaks(
        "module M; "
        "class SC: IEquatable<string> "
        "  public method Equals(other: string): bool => other = \"mg2766-heap-xyz\"; "
        "end SC; "
        "class C public static method Main(): int "
        "  using var sc := new SC(); "
        "  var e: IEquatable<string> := sc; "
        "  var f := e.Equals; "
        "  var a := \"mg2766-heap\"; "
        "  var b := a + \"-xyz\"; "
        "  if f(b) then return 43; end; "
        "  return 3; "
        "end Main; end C;"), 43);
}
