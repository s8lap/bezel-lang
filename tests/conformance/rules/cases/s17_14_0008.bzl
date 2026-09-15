//! case: s17_14_0008
//! doc: spec
//! section: 17.14
//! title: an interpolated string is not a constant expression
//! rule: "Nothing else is constant. In particular: ... interpolated strings
//!   ... are not constant expressions. `const S := $\"v{H}\";` [is a]
//!   compile error: interpolated strings are not constant."
//! cites: 17.14
//! expect: reject
//! diagnostic: constant
module Conf;

public class Program
  public static method Main()
    const h := 5;
    const s := $"v{h}";
  end Main;
end Program;
