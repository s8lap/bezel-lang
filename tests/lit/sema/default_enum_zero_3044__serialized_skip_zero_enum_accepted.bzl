// fix #3044 (§9.8 / §17.11, v0.242) OVER-REJECTION GUARD: `serialized skip` on
// an enum component that DOES declare a zero-valued constant stays accepted —
// `default(Level)` is `Off`, so the synthesized `Deserialize` has a value to
// reconstruct with. Pairs with
// default_enum_zero_3044__serialized_skip_no_zero_rejected.bzl; the only
// difference is `Off := 0`.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
import System.Serialization;

enum Level Off := 0, Low := 1, Mid := 2; end Level;

record Doc(Title: string, Rank: Level serialized skip): ISerializable, IDeserializable;

class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
