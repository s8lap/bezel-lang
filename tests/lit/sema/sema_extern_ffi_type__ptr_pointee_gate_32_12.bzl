// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Regression for issue #4561 (spec §32.5 / §32.12, v0.306).
//
// §32.5 admits `Ptr<T>` at the boundary — "admitted as a parameter, as a
// return type, and inside FFI function types" — but §32.12 clause 1 gates the
// POINTEE: "`T` is drawn from the FFI-pointee set — integer types, `float`,
// `double`, `bool`, `IntPtr`, `UIntPtr`, and C-compatible records.
// `Ptr<Ptr<U>>` is not admitted (v1 decline …)". stdlib §10.5 requires the
// rejection to name the set.
//
// The companion accept side (`Ptr<int>` / `Ptr<TimeSpec>` in all three
// positions) is in tests/codegen/ffi_c_compatible_records_4561_test.cpp.

module M;
import System.Interop;

public class Widget
  public constructor()
  end constructor;
end Widget;

public record Config(Name: string, Retries: int);

extern "libexample"
  public method nested(p: Ptr<Ptr<int>>): int;
  public method atClass(p: Ptr<Widget>): int;
  public method atNonCCompatibleRecord(p: Ptr<Config>): int;
  public method nullablePtr(p: Ptr<int>?): int;
end;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: extern method parameter `p` has type `Ptr<Ptr<int>>`, whose pointee is not in the FFI-pointee set
// CHECK-SAME: `Ptr<Ptr<U>>` is not admitted
// CHECK-SAME: (§32.12)
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: extern method parameter `p` has type `Ptr<Widget>`, whose pointee is not in the FFI-pointee set
// CHECK-SAME: (§32.12)
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: extern method parameter `p` has type `Ptr<Config>`, whose pointee is not in the FFI-pointee set
// CHECK-SAME: (§32.12)
// §32.5 "Nullable value types are rejected: … `Ptr<T>?` …" — the nullable
// form never reaches the pointee gate; it is rejected as a nullable value type.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: extern method parameter `p` has type `Ptr<int>?`, which is not permitted at the FFI boundary
// CHECK-SAME: (§32.5)
