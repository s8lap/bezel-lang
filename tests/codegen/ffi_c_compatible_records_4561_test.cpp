// narrow #4561 — spec §32.5 / §32.12 / §32.13 (v0.306).
//
// THE RULES, as the rule corpus states them (`s32_5_0002`, `s32_13_0001`,
// `s32_13_0003` — all three `accept`-tier divergences where the spec accepts
// and the compiler rejected):
//
//   §32.5   "`Ptr<T>` — a typed foreign address (§32.12); admitted as a
//            parameter, as a return type, and inside FFI function types,
//            marshalling as the pointer it is."
//   §32.13  "A record is C-compatible iff every field's type is an
//    clause 1  FFI-compatible scalar …"
//   §32.13  "Admission — by address (§32.5): as `ref`/`out` extern parameters
//    clause 3  (C sees `R*`) …"
//   §32.13  "Typed handles: the single-field admission. A C-compatible record
//    clause 4  with exactly one field, of integer or pointer scalar type … is
//              FFI-compatible directly — parameter, return, `ref`/`out`, and
//              callback positions — marshalling exactly as its field."
//
// THE DEFECT. `Resolver::isFfiCompatibleType` had no arm for either shape: a
// `Ptr<T>` and every record reached the catch-all "not permitted at the FFI
// boundary (§32.5)". The declarations simply did not compile.
//
// WHY THE ORACLE IS A VALUE, NOT ACCEPTANCE. Every `accept` case in the §32
// rule corpus is `shape: lib` — a declaration that is never called — so
// admitting the type would turn the corpus row green whether or not the
// marshalling is right. These rows CALL real libc functions whose results are
// known exactly, so a wrong slot (by value where C wants `R*`, or the record's
// address where C wants the field) is a wrong printed value, not a green row.
// The §32.5 `ref`/`out` address passing those rows depend on is #1706.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// §32.13 clauses 1+3 — the spec's own worked example, verbatim in shape:
// `public record TimeSpec(Sec: long, Nsec: long);` is C-compatible (two
// integer scalars) and crosses as `struct timespec*` at an `out` parameter.
// CLOCK_MONOTONIC is 1 on Linux; the kernel fills tv_sec with the uptime and
// tv_nsec with a value in [0, 1e9). A by-value lowering here would hand
// clock_gettime two register-sized halves instead of a pointer.
BEZEL_TEST(codegen_ffi_c_records,
           c_compatible_record_crosses_by_address_at_out_spec_32_13_4561) {
    auto r = runAot(
        "module M; "
        "public record TimeSpec(Sec: long, Nsec: long); "
        "extern \"libc\" method clock_gettime(clockId: int, "
        "                                     out ts: TimeSpec): int; end; "
        "class C public static method Main(): int "
        "  var ts: TimeSpec := new TimeSpec(0L, 0L); "
        "  var rc := clock_gettime(1, out ts); "
        "  var ok := 0; "
        "  if ts.Sec > 0L then ok := ok + 1; end; "
        "  if ts.Nsec >= 0L then ok := ok + 2; end; "
        "  if ts.Nsec < 1000000000L then ok := ok + 4; end; "
        "  Console.WriteLine($\"rc={rc} ok={ok}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("rc=0 ok=7\n"));
}

// §32.13 clause 4 — the typed handle, in BOTH the by-value parameter and the
// return slot, "marshalling exactly as its field". `int dup(int)` duplicates
// stdout: the result is a fresh descriptor > 2. If the record were passed by
// address, dup would receive a stack address as its fd and return -1 (EBADF).
BEZEL_TEST(codegen_ffi_c_records,
           single_field_handle_record_by_value_and_return_spec_32_13_4561) {
    auto r = runAot(
        "module M; "
        "public record Fd(Raw: int); "
        "extern \"libc\" "
        "  method dup(oldfd: Fd): Fd; "
        "  method close(fd: Fd): int; "
        "end; "
        "class C public static method Main(): int "
        "  var d := dup(new Fd(1)); "
        "  var ok := 0; "
        "  if d.Raw > 2 then ok := 1; end; "
        "  var rc := close(d); "
        "  Console.WriteLine($\"ok={ok} rc={rc}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok=1 rc=0\n"));
}

// §32.13 clause 4 at a boundary `out` — the `char**` shape written as a typed
// handle instead of a bare `IntPtr` (§32.13's whole point: "passing a
// statement where a database belongs is a compile error"). strtol parses "42"
// and leaves endptr pointing at "rest", so the handle is non-null after the
// call. (Plain `runAot`: pre-fix the declaration does not compile at all, so
// the value oracle already separates the two sides; the gate's `--full` leg
// runs this same row under generated-code ASan.)
BEZEL_TEST(codegen_ffi_c_records,
           single_field_handle_record_at_out_parameter_spec_32_13_4561) {
    auto r = runAot(
        "module M; "
        "public record CharPtr(Raw: IntPtr); "
        "extern \"libc\" method strtol(nptr: CString, out endptr: CharPtr, "
        "                              radix: int): long; end; "
        "class C public static method Main(): int "
        "  using var s := CString.From(\"42rest\"); "
        "  var p: CharPtr := new CharPtr(IntPtr.Zero); "
        "  var v := strtol(s, out p, 10); "
        "  var set := 0; "
        "  if p.Raw <> IntPtr.Zero then set := 1; end; "
        "  Console.WriteLine($\"v={v} set={set}\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("v=42 set=1\n"));
}

// §32.5 — `Ptr<T>` in all three admitted positions at once (the rule corpus's
// `s32_5_0002`, verbatim), plus a `Ptr<R>` over a C-compatible record pointee
// (§32.12 clause 1 admits those; `s32_12_0004`'s declaration half). There is
// no runtime observable to assert: stdlib §10.5's member surface
// (`Zero`/`Load`/`Store`/`Add`/…) is not implemented by this compiler, so no
// `Ptr<T>` VALUE can be produced — the rule these declarations state is
// admission, and the program's own exit status shows the module compiled,
// linked and ran with the externs present.
BEZEL_TEST(codegen_ffi_c_records,
           ptr_admitted_as_param_return_and_in_fn_type_spec_32_5_4561) {
    auto r = runAot(
        "module M; "
        "import System.Interop; "
        "public record TimeSpec(Sec: long, Nsec: long); "
        "extern \"libexample\" "
        "  method FindFirst(data: Ptr<int>, count: int, "
        "      pred: (Ptr<int>) -> bool): Ptr<int>; "
        "  method PeekTime(p: Ptr<TimeSpec>): int; "
        "end; "
        "class C public static method Main(): int "
        "  Console.WriteLine(\"declared\"); "
        "  return 7; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("declared\n"));
}
