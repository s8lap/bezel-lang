//! case: s22_13_0013
//! doc: spec
//! section: 22.13
//! title: a static readonly class-typed field without owned is exempt from the ownership-discharge requirement
//! rule: A `static readonly` field of class type or interface type that
//!   is not marked `owned` is exempt from the ownership-discharge
//!   requirement: assigning a fresh owned value into such a field
//!   discharges the producer. The value lives for the duration of the
//!   program.
//! cites: 22.13
//! expect: accept
module Conf;

public class Registry
  private static readonly _cts: CancellationTokenSource := new CancellationTokenSource();
end Registry;
