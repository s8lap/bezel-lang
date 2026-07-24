// RUN: %bezelc -c -o %t.o %s
//
// §9.7 / §35.4.3 (v0.226, issue #2210): the ACCEPT (non-over-rejection)
// control. A record whose choice field is VALUE-LIKE (`VOpt` has only
// `int`/`string` payloads, §35.4.3) is NOT owning — the value-shaped
// storage has no disposal obligation, and the record is a freely usable
// value type. The §35.4 classifier must gate the rejection on Owning;
// widening to "any choice field" would reject this case and is wrong.

module M; choice VOpt Some(n: int), Named(s: string), None; end VOpt; record HolderV(K: VOpt); class C public static method Main(): int var h := new HolderV(VOpt.Some(42)); return 0; end Main; end C;
