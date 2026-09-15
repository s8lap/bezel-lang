//! case: s23_1_0003
//! doc: spec
//! section: 23.1
//! title: a constructor may not write another instance's readonly auto-property
//! rule: The assignment is admitted only through `this` for an instance
//!   auto-property — the assignable context is per-instance, so a constructor may
//!   not write another value's readonly storage.
//! cites: 23.1, 22.8
//! expect: reject
//! diagnostic: readonly
module Conf;

public class Account
  public readonly property Number: string;

  public constructor(n: string)
    Number := n;
  end constructor;

  public constructor(other: Account, n: string)
    Number := n;
    other.Number := n;
  end constructor;
end Account;
