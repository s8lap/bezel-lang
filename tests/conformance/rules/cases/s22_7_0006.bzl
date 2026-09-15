//! case: s22_7_0006
//! doc: spec
//! section: 22.7
//! title: a virtual call on this from a non-sealed class's constructor body is rejected
//! rule: "No virtual dispatch from constructor bodies of non-sealed
//!   classes. A call from a constructor body to a `virtual`, `override`,
//!   or `abstract` member with receiver `this` ... is a compile error
//!   unless the declaring class is `sealed` and not `abstract`."
//! cites: 22.7
//! expect: reject
//! diagnostic: virtual
module Conf;

public class Animal
  public constructor()
    this.Speak();
  end constructor;

  public virtual method Speak()
  end Speak;
end Animal;
