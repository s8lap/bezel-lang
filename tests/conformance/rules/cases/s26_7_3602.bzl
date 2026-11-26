//! case: s26_7_3602
//! doc: spec
//! section: 26.7
//! title: an extension over string stores a freshly built receiver into a record, 200 times
//! rule: For an extended type that is **runtime-managed** (§28.9: `string`, `Type`; …), the three bans
//!   have no ownership to protect, and `this` is read as a plain parameter of that type: … so storing it —
//!   into a field, a record or choice component (`new Named(this, 1)` in `extension string`), a
//!   collection, a capture — is the store of a runtime-managed value, admitted wherever storing such a
//!   parameter is
//! cites: 26.7, 28.9
//! expect: run
//! stdout: 690
//! issue: 8113
//! notes: The receiver is a call result ("s" + i). Lengths: i < 10 gives 2 (10 x 2), i < 100 gives 3
//!   (90 x 3), i < 200 gives 4 (100 x 4): 20 + 270 + 400 = 690.
module Conf;

public record Named(Tag: string, N: int);

public extension string
  public method Wrap(): Named => new Named(this, 1);
end extension;

public class Make
  public static method Str(n: int): string => "s" + n.ToString();
end Make;

public class Program
  public static method Main()
    var t := 0;
    for var i in 0 ..< 200 do
      t := t + Make.Str(i).Wrap().Tag.Length;
    end for;
    Console.WriteLine(t);
  end Main;
end Program;
