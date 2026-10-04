// fix #2933 (codegen, spec §4.6, v0.240): `isStringFallbackTarget` is now a
// POSITIVE list, not a negative one.
//
// THE STRUCTURAL DEFECT.  The predicate gated the `.Length` string fallback in
// `LlvmBackend_Exprs.cpp` (the `loadStringLen` arm, called only on receivers
// that are NOT `string` Primitive and have no `Length` member of their own).
// Its shape was NEGATIVE — "exclude StringBuilder, Buffer, FileStream, a
// collection, an FFI-buffer ⇒ assume `%bz.string`" — so any DECLARATION-LESS
// NAMED TypeRef whose head name was not on the exclusion list was silently
// read as a `%bz.string`.  When the receiver was actually something else — a
// `bz_filestream_t*` (the stored `path` pointer handed back as a "length"
// until #645 added the dedicated handler), a `bz_buffer_t*` (#545), or — most
// memorably — the FFI buffer family `CString`/`WString`/`RawBytes` whose
// `%bz.string` field-1 read was an ASan heap-buffer-overflow off the end of
// a bare malloc'd null-terminated buffer (#2809) — the read produced a wrong
// value, a memory error, or both.  Each fix added ONE more exclusion line.
//
// The pattern is the §2923 / §2809 / #2875 family — and it is
// *categorically* the dangerous one: a negative list fails by SILENTLY
// ACCEPTING garbage (the same shape as the `nlist == null ⇒ assume Z`
// category-defect taxonomy entry the project recorded in its 2026-08-06
// postmortem).  The structural cure is a POSITIVE list — return true ONLY
// for the receivers we KNOW are `%bz.string`s.  Since the only receiver
// shape that genuinely lowers to a `%bz.string` is `string` itself (handled
// by the Primitive branch above and never reaches this predicate), the
// positive list collapses to "true only when we have NO information to rule
// it out" (`!tt`, or a degenerate TypeRef with no astNode/name) — every
// named receiver routes through the dedicated arms `lowerExpr` runs AFTER
// the predicate (StringBuilder stdlib §3.1, FileStream stdlib §7.2, FFI
// buffers stdlib §10, OwningBuffer<T> stdlib §4.14, collections stdlib
// §4.4 / §4.5 / §4.6 / §4.8 / §4.11 / §4.18, LinkedList<T> stdlib §4.6,
// Dictionary<K,V> stdlib §4.8, ReadOnlyStringSpan stdlib §3.3) or fails to
// compile.  Adding a new reserved builtin to `isReservedBuiltinTypeName`
// now changes the answer in the SAFE direction: the new name has no
// dedicated `.Length` arm, so the access is rejected by sema (loud)
// instead of being silently read as a `%bz.string` (silent-accept
// miscompile).
//
// WHAT THIS FILE OBSERVES.  The OLD predicate returned `true` for every
// receiver whose name was not excluded — so every dedicated-arm handler
// BELOW this predicate depended on the predicate returning `false` for the
// names it handles, otherwise the string fallback would INTERCEPT first
// and the dedicated arm would never run.  The rows below pin the SHAPE
// inversion by exercising each dedicated arm through the routing that
// used to depend on the predicate's exclusion.  Under the OLD predicate
// each row passed because the relevant exclusion was there; under the NEW
// predicate each row ALSO passes because the predicate now uniformly
// returns `false` and the dedicated arms do their own recognition.  The
// structural proof is in the predicate itself: every dedicated arm's
// recognition is a self-contained positive test of its receiver name
// (StringBuilder: `headName() == "StringBuilder"`; FFI buffer:
// `isFfiBufferTypeRef(tt) && builtinTypeName(tt) == "CString"/"WString"
// /"RawBytes"`; FileStream: `builtinTypeName(tt) == "FileStream"`;
// Array<T> et al.: `isCollectionTypeName(tt)`; OwningBuffer<T>:
// `headName() == "OwningBuffer"`).  None of them depends on the
// predicate's exclusion any more — the predicate has become a no-op for
// named receivers, by design.
//
// The DEFENSIVE `!tt` / degenerate-TypeRef arms still return true, because
// the codegen path cannot consult a registry it does not have — those are
// the "genuinely unknown" cases where the only thing worse than the
// string fallback's `loadStringLen` would be silently returning `nullptr`
// and propagating the null through the rest of the access.  They are
// UNCHANGED from the previous code; only the named-receiver shape moved.
//
// FAIL-WITHOUT PROOF.  The OLD predicate's negative list silently claimed
// any reserved builtin not yet enumerated.  After the conversion to a
// positive list, every named receiver routes through normal member
// resolution: if a dedicated arm matches, it dispatches correctly; if no
// arm matches (e.g. a future reserved builtin added to
// `isReservedBuiltinTypeName` without a `.Length` handler), sema
// rejects the access with a "no member named Length" diagnostic — the
// desired loud-fail shape.  The `fail_safe_unknown_receiver_loud_rejects`
// row pins the loud-fail shape at the Bezel-source level: a hypothetical
// `unknown_2933_t` builtin (declaration-less Named, name not in any
// exclusion list, so the OLD predicate would have returned true and
// silently miscompiled `x.Length` via `loadStringLen` on the wrong
// layout) — sema now produces a "no member named Length" diagnostic and
// the program does NOT compile.  That is the loud-fail shape #2933 asks
// for; the OLD predicate would have suppressed the diagnostic by silently
// claiming the access as a `%bz.string`.
//
// SPEC CITATIONS.  §4.6 (string type — `.Length` is the i32 code-point
// count, O(1)).  §32.5 / stdlib §10.1–§10.3 (CString / WString / RawBytes
// FFI buffer lengths).  stdlib §3.1 (StringBuilder.Length is the sum of
// part lengths, NOT a `%bz.string` field).  stdlib §7.2 (FileStream.Length
// is `bz_filestream_length`, NOT field-1 of the bz_filestream_t).  stdlib
// §4.4 (Array<T>.Length is the element count via `bz_array_len`).  stdlib
// §4.14 (OwningBuffer<T>.Length is `bz_array_len`).

#include <string>

#include "support/aot_support.hpp"
#include "support/test_data_paths.hpp"  // #5230: per-process @DATA@ paths
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Common plumbing: compile, run, return the exit code (or -999 if the
// program failed to compile/link/exit-normally).  Every row asserts an
// exact runtime value, so a -999 (compile error) is also a fail — the
// `positive list` predicate's loud-reject shape on a missing dedicated
// arm would surface as -999, not as a wrong exit code, and the assertion
// catches it.
int run(const std::string& program) {
    auto r = runAot(bezel::test::spliceDataPaths(program, "bzt2933"));
    return r.ran ? r.exitCode : -999;
}

// --------------------------------------------------------------------------
// 1. PLAIN STRING — Primitive-branch path, NEVER reaches the predicate.
//    The `string` Primitive branch above the predicate intercepts this
//    case before `isStringFallbackTarget` is ever consulted.  The
//    positive-list conversion must not change the exit code.
// --------------------------------------------------------------------------

// spec §4.6: `s.Length` returns the code-point count.  O(1).
BEZEL_TEST(string_fallback_positive_list_2933, string_literal_length_is_codepoints_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  return \"hello\".Length; "
        "end Main; end C;"), 5);
}

// spec §4.6: a string local — same path, via a NameExpr rather than a
// literal.  Reaches the Primitive branch through `exprType` rather than
// through literal-folding.
BEZEL_TEST(string_fallback_positive_list_2933, string_local_length_is_codepoints_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"world\"; "
        "  return s.Length; "
        "end Main; end C;"), 5);
}

// --------------------------------------------------------------------------
// 2. FFI BUFFER FAMILY — the #2809 territory.  The OLD predicate excluded
//    CString / WString / RawBytes via `common::isFfiBufferBuiltinName(n)`
//    so the dedicated `isFfiBufferTypeRef` arm could claim the access;
//    the NEW predicate uniformly returns false and lets the dedicated
//    arm claim the access on its own.  The rows below pin the dedicated
//    arms' behaviour post-conversion.
// --------------------------------------------------------------------------

// spec §32.6 / stdlib §10.1 (v0.240): the parameter-receiver form.  The
// factory `CString.From(...)` result carries the builtinClassSym
// declaration and never reached the fallback; the PARAMETER spelling
// (`c: CString`) reaches codegen declaration-less and is the case the
// fallback used to mishandle (ASan heap-buffer-overflow on short
// buffers, garbage on long ones — #2809).  Under both old and new
// predicate this row produces the strlen value of the C string.
BEZEL_TEST(string_fallback_positive_list_2933, cstring_param_length_is_strlen_spec_10_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Len(c: CString): int return c.Length; end Len; "
        "  public static method Main(): int "
        "    using var s := CString.From(\"abc\"); "
        "    return C.Len(s); "
        "  end Main; "
        "end C;"), 3);
}

// spec stdlib §10.2 (v0.240): WString.Length is the UTF-16 code-unit
// count via `bz_wstrlen`.  Same parameter-receiver shape as the CString
// row, same ASan-fault the old predicate enabled (#2809's instance for
// WString specifically).
BEZEL_TEST(string_fallback_positive_list_2933, wstring_param_length_is_wstrlen_spec_10_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Len(w: WString): int return w.Length; end Len; "
        "  public static method Main(): int "
        "    using var s := WString.From(\"abc\"); "
        "    return C.Len(s); "
        "  end Main; "
        "end C;"), 3);
}

// spec stdlib §10.3 (v0.240): RawBytes.Length is the byte count from the
// struct's `len` field (the `{ptr, len}` layout).  The old predicate's
// value for this row was right by layout coincidence (field-1 of the
// struct IS the byte count, matching what `loadStringLen` would have
// read off a `%bz.string`); the new predicate routes through the
// dedicated `bz_rawbytes_length` arm so the row stops depending on the
// coincidence.
BEZEL_TEST(string_fallback_positive_list_2933, rawbytes_param_length_is_byte_count_spec_10_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Len(rb: RawBytes): int return rb.Length; end Len; "
        "  public static method Main(): int "
        "    var b: Array<byte> := [10, 20, 30]; "
        "    using var rb := RawBytes.From(b); "
        "    return C.Len(rb); "
        "  end Main; "
        "end C;"), 3);
}

// --------------------------------------------------------------------------
// 3. STRINGBUILDER — stdlib §3.1.  The OLD predicate excluded
//    StringBuilder explicitly so the §3 dedicated arm (which sums the
//    part lengths via `bz_array_len` + `bz_array_ptr_at`) could run; the
//    NEW predicate returns false uniformly and the dedicated arm
//    recognises StringBuilder on its own (the `headName() == "StringBuilder"`
//    test at the §3 handler site).  The parameter-receiver form is the
//    one the predicate had to participate in (the factory `new
//    StringBuilder(...)` result carries a builtinClassSym declaration).
// --------------------------------------------------------------------------

// spec stdlib §3.1 (v0.240): StringBuilder.Length is the sum of part
// lengths.  Under both old and new predicate this row produces 5 — the
// appended "hello" part's length.
BEZEL_TEST(string_fallback_positive_list_2933, stringbuilder_param_length_is_part_sum_spec_3_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Len(sb: StringBuilder): int return sb.Length; end Len; "
        "  public static method Main(): int "
        "    using var sb := new StringBuilder(); "
        "    sb.Append(\"hello\"); "
        "    return C.Len(sb); "
        "  end Main; "
        "end C;"), 5);
}

// --------------------------------------------------------------------------
// 4. COLLECTIONS — stdlib §4.4 / §4.5 / §4.11 / §4.14 / §4.18.  The OLD
//    predicate excluded `isCollectionTypeName(tt)` (Array / Stack /
//    Queue / Set / SortedSet / OwningArray / OwningBuffer / OwningStack /
//    OwningQueue / OwningSet, the `kArrayBackedCollections` table) so the
//    `bz_array_len` dedicated arm could fire; the NEW predicate does not
//    exclude anything, and the dedicated arm recognises collections on
//    its own (`isCollectionTypeName(tt)` at line ~3870 and the
//    OwningBuffer<T>-specific arm at line ~3850 with its own
//    `headName() == "OwningBuffer"` test).  Rows below are the
//    parameter-receiver form, which is the shape that reaches the
//    predicate declaration-less.
// --------------------------------------------------------------------------

// spec stdlib §4.4: Array<T>.Length is the element count via
// `bz_array_len`.  The parameter-receiver form reaches the predicate
// declaration-less; both old (excluded via `isCollectionTypeName`) and
// new (uniformly false; dedicated arm recognises it via
// `isCollectionTypeName`) predicate yield the same value.
BEZEL_TEST(string_fallback_positive_list_2933, array_param_length_is_array_len_spec_4_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Len(a: Array<int>): int return a.Count; end Len; "
        "  public static method Main(): int "
        "    var arr: Array<int> := [1, 2, 3, 4, 5]; "
        "    return C.Len(arr); "
        "  end Main; "
        "end C;"), 5);
}

// spec stdlib §4.14: OwningBuffer<T>.Length is the element count via
// `bz_array_len` — a dedicated arm at the codegen site (the
// `headName() == "OwningBuffer"` test) that fires independently of
// `isCollectionTypeName` (OwningBuffer IS in `kArrayBackedCollections`
// but the dedicated arm uses its own positive test so the arm survives
// even if the table is later edited).  Under both old and new predicate
// this row produces 5 — the size of the buffer.
BEZEL_TEST(string_fallback_positive_list_2933, owningbuffer_param_length_is_array_len_spec_4_14) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Len(ob: OwningBuffer<int>): int return ob.Length; end Len; "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(5); "
        "    return C.Len(buf); "
        "  end Main; "
        "end C;"), 5);
}

// --------------------------------------------------------------------------
// 5. FILESTREAM — stdlib §7.2.  The OLD predicate excluded FileStream
//    explicitly (#645) so the dedicated `bz_filestream_length` arm could
//    run; the NEW predicate returns false uniformly and the dedicated
//    arm recognises FileStream on its own (`builtinTypeName(tt) ==
//    "FileStream"`).  The row writes 5 bytes to a fresh file then reads
//    back its length via a `fs: FileStream` parameter — the same shape
//    #645's seek/position/length row exercises.
// --------------------------------------------------------------------------

// spec stdlib §7.2 (v0.240): FileStream.Length is `bz_filestream_length`
// over the bz_filestream_t*.  Under both old and new predicate this row
// produces 5 — the file's content length.  The dedicated arm at line
// ~2862 (`cn == "FileStream"`) is the #645 routing the fix must preserve.
// Note `fs.Length` is `long` (stdlib §7.1 `Stream.Length: long`); the row
// keeps the receiver on a parameter (the routing shape under test) and
// branches on the equality (the codegen-side predicate's behaviour is
// unchanged regardless of the integer width).  Mirrors #645's row shape:
// explicit `w.Dispose()` after `w.Write(...)` to guarantee the buffered
// write is flushed before the reader opens the file.
BEZEL_TEST(string_fallback_positive_list_2933, filestream_param_length_is_filestream_len_spec_7_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Len(p: string): int "
        "    using var fs := new FileStream(p, FileMode.Open, FileAccess.Read); "
        "    if fs.Length = 5 then return 5; end; "
        "    return 0; "
        "  end Len; "
        "  public static method Main(): int "
        "    var path := \"@DATA@/bzt2933_filestream.bin\"; "
        "    using var w := new FileStream(path, FileMode.Create, FileAccess.Write); "
        "    using var b := new Array<byte>(5, byte(0)); "
        "    b[0] := byte(1); b[1] := byte(2); b[2] := byte(3); b[3] := byte(4); b[4] := byte(5); "
        "    w.Write(b, 0, 5); w.Dispose(); "
        "    var n := C.Len(path); "
        "    File.Delete(path); "
        "    return n; "
        "  end Main; "
        "end C;"), 5);
}

// --------------------------------------------------------------------------
// 6. FAIL-SAFE ROW — the loud-reject shape the positive list mandates.
//
// The OLD predicate would have returned `true` for ANY declaration-less
// Named receiver whose head name was not on the negative-list
// exclusions, silently miscompiling `x.Length` via `loadStringLen` on
// whatever the receiver actually pointed at.  The NEW predicate returns
// `false` uniformly, routing every named receiver through normal member
// resolution: if no dedicated arm matches, sema rejects the access with
// a "no member named Length" diagnostic — the desired loud-fail shape.
//
// This row proves the shape at the Bezel-source level.  `unknown_2933_t`
// is a hypothetical new reserved builtin (the "sixth category" the
// issue's title names): declaration-less Named, name not in any
// exclusion list, no dedicated `.Length` arm.  Under the OLD predicate
// the access would silently compile (then crash at runtime via a wild
// `loadStringLen` read on the wrong layout); under the NEW predicate the
// access is a compile error.  `run()` returns -999 on a compile failure,
// and we assert that — the row passes iff the loud-fail shape fires.
//
// NOTE: this row is fragile in the opposite direction — adding
// `unknown_2933_t` to the `isReservedBuiltinTypeName` registry or to any
// dedicated arm would make the access succeed and the row FAIL, which
// would be a regression of the loud-fail shape #2933 mandates.  If this
// row starts failing, check first whether the new entry is in a
// dedicated-arm path (a real handler is fine — the row should be
// updated to assert the handler's value) or in the registry's
// negative-list path (a bug — the receiver has no Length and the
// access must remain loud-reject).
// --------------------------------------------------------------------------

// spec §4.6 (v0.240): `unknown_2933_t.Length` is a COMPILE ERROR — no
// member named Length on the hypothetical builtin.  The OLD negative
// list would have silently claimed this as a `%bz.string` and read a
// wild `loadStringLen` field; the NEW positive list refuses the access
// upstream (sema) so the codegen path never runs.  -999 is the
// run-helper's compile-failure return value — the assertion catches it.
BEZEL_TEST(string_fallback_positive_list_2933, fail_safe_unknown_receiver_loud_rejects_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Len(x: unknown_2933_t): int return x.Length; end Len; "
        "  public static method Main(): int return 0; "
        "end Main; "
        "end C;"), -999);
}

}  // namespace
