//! case: s17_10_0003
//! doc: spec
//! section: 17.10
//! title: typeof(this) is a compile error
//! rule: "`typeof(this)` and `typeof(base)` are compile errors. `this` and
//!   `base` are values, not type names. To obtain the runtime type of the
//!   receiver, use `this.GetType()`."
//! cites: 17.10
//! expect: reject
//! diagnostic: not type names
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Describe()
    var t: Type := typeof(this);
    Console.WriteLine(t.Name);
  end Describe;
end Widget;
