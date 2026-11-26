//! case: s26_7_3601
//! doc: spec
//! section: 26.7
//! title: an extension over string may store this into a record
//! rule: For an extended type that is **runtime-managed** (§28.9: `string`, `Type`; …), the three bans
//!   have no ownership to protect, and `this` is read as a plain parameter of that type: … so storing it —
//!   into a field, a record or choice component (`new Named(this, 1)` in `extension string`), a
//!   collection, a capture — is the store of a runtime-managed value, admitted wherever storing such a
//!   parameter is
//! cites: 26.7, 28.9
//! expect: run
//! stdout: abc
//! issue: 8113
module Conf;

public record Named(Tag: string, N: int);

public extension string
  public method Wrap(): Named => new Named(this, 1);
end extension;

public class Program
  public static method Main()
    Console.WriteLine("abc".Wrap().Tag);
  end Main;
end Program;
