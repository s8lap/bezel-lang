// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Regression for issue #4561 (spec §32.13, v0.306).
//
// §32.13 clause 1 computes the C-compatible property: "a record is
// C-compatible iff every field's type is an FFI-compatible scalar …, an inline
// array `[N]T` of such a scalar …, or another C-compatible record". Clause 3
// admits one "by address only: as `ref`/`out` extern parameters … and as
// `Ptr<R>` pointees. By value is not admitted, except the single-field form".
// Clause 4's single-field form requires the one field to be of "*integer or
// pointer* scalar type … — not `float`/`double`, not `bool`", the restriction
// being ABI-driven ("`struct { double }` diverges" between SysV and Win64).
//
// Three rejections, each of which used to read "has type `X`, which is not
// permitted at the FFI boundary (§32.5)" — true, but naming neither the
// property that failed nor the spelling that would work.
//
// The accept side is in tests/codegen/ffi_c_compatible_records_4561_test.cpp.

module M;

public record Labeled(Name: string, Value: int);   // `string` field: not C-compatible
public record TimeSpec(Sec: long, Nsec: long);     // C-compatible, two fields
public record Measurement(Value: double);          // one field, but `double`

extern "libexample"
  public method notCCompatible(ref item: Labeled): int;
  public method byValueMultiField(ts: TimeSpec): int;
  public method singleFieldDouble(m: Measurement): int;
end;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: extern method parameter `item` has record type `Labeled`, which is not C-compatible
// CHECK-SAME: (§32.13)
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: extern method parameter `ts` has record type `TimeSpec`, which is C-compatible but admitted by address only
// CHECK-SAME: declare the parameter `ref` or `out`, or take a `Ptr<TimeSpec>`
// CHECK-SAME: (§32.13)
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: extern method parameter `m` has record type `Measurement`, which is C-compatible but admitted by address only
// CHECK-SAME: single-field form
// CHECK-SAME: (§32.13)
