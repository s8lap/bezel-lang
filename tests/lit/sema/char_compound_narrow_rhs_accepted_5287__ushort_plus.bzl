// RUN: %bezelc -c -o %t.o %s
//
// §6.6, v0.313 (#5287) — see char_compound_narrow_rhs_accepted_5287__byte_plus.bzl
// for the rule. `ushort` is the last of the four narrow types §6.6 admits
// alongside `sbyte`, `byte` and `short`.

module M; class C public static method Main(): int var c: char := char(50); var b: ushort := ushort(65535); c += b; return int(c); end Main; end C;
