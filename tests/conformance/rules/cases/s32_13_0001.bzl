//! case: s32_13_0001
//! doc: spec
//! section: 32.13
//! title: a record of only FFI-compatible scalar fields is C-compatible and admitted by address
//! rule: A record is C-compatible iff every field's type is an FFI-compatible scalar
//!   ... Admission -- by address: as `ref`/`out` extern parameters.
//! cites: 32.13, 32.5
//! expect: accept
module Conf;

public record TimeSpec(Sec: long, Nsec: long);

extern "libc"
  method clock_gettime(clockId: int, out ts: TimeSpec): int;
end;
