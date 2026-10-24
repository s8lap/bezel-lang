// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// The CALL spelling of the same rejection as
//   sema_unknown_primitive_method_spec3_4__string_empty_is_rejected_7128.bzl
// (the READ spelling). `string.Empty()` was accepted by sema and lowered to
// the empty string literal, but `Empty` is not a stdlib §9.6 member — the
// `String` listing declares no `Empty`, and `Bezel_For_Code_Generation.md` §5
// lists `String.Empty` among the APIs that do NOT exist, with the
// replacement "use `""`".
//
// fix #7128 drops `Empty` from the string static-member accept set, so the
// CALL spelling falls through to the clean §3.4 "type 'string' has no static
// method or property 'Empty'" reject too.
//
// §3.4 / stdlib §9.6: `string` has no static member `Empty`.

module M; class C public static method Main(): int var s := string.Empty(); return s.Length; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type 'string' has no static method or property 'Empty'
// CHECK-SAME: (§3.4)
