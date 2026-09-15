//! case: s23_1_0002
//! doc: spec
//! section: 23.1
//! title: a readonly auto-property may not be assigned from an ordinary method
//! rule: A `readonly` auto-property may be assigned only in a field initializer or
//!   in a constructor, under §22.8's per-instance assignable context.
//! cites: 23.1, 22.8
//! expect: reject
//! diagnostic: readonly
module Conf;

public class Account
  public readonly property Number: string;

  public constructor(n: string)
    Number := n;
  end constructor;

  public method Rename(n: string) => Number := n;
end Account;
