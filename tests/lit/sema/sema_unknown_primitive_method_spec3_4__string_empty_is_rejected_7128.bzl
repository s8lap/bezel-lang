// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Inverted by fix #7128. This row was
//   sema_unknown_primitive_method_spec3_4__string_empty_still_accepted_1034.bzl
// and asserted the OPPOSITE: that `string.Empty` compiles CLEAN (a
// "NODIAG" accept control for the #1034 `string.Format` / `string.Concat`
// removal). The member is NOT a stdlib §9.6 member — the `String` listing
// declares no `Empty` at all, and `Bezel_For_Code_Generation.md` §5 lists
// `String.Empty` among the APIs that do NOT exist, with the replacement
// "use `""`" — so the accept set that carried it is the DEFECT (#7128), not
// the contract.
//
// The READ spelling was the crashing one: it had no codegen lowering, so the
// local it initialised was left zero-initialized (a NULL bz.string) and the
// program's first read of it dereferenced NULL at offset 0x8 — the string
// header's length field — killing the generated program with SIGSEGV. The
// remedy is the REJECTION: `Empty` is dropped from the string static-member
// accept set, so both spellings fall through to the clean §3.4
// "type 'string' has no static method or property 'Empty'" reject, exactly
// as any other unknown static (`string.Bogus()`).
//
// The companion row for the CALL spelling is
//   sema_unknown_primitive_method_spec3_4__string_empty_call_is_rejected_7128.bzl
//
// §3.4 / stdlib §9.6: `string` has no static member `Empty`.

module M; class C public static method Main(): int var s := string.Empty; return s.Length; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type 'string' has no static method or property 'Empty'
// CHECK-SAME: (§3.4)
