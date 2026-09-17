//! case: s17_10_0004
//! doc: spec
//! section: 17.10
//! title: typeof does not accept a variable or a literal
//! rule: "`typeof` does not accept variables, method names, or literals —
//!   those are compile errors. For the runtime type of a *value*, use
//!   `value.GetType()`."
//! cites: 17.10, 8.2
//! expect: reject
//! diagnostic: typeof
module Conf;

public class Program
  public static method Main()
    var x := 5;
    var t: Type := typeof(x);
  end Main;
end Program;
