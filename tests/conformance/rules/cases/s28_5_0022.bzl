//! case: s28_5_0022
//! doc: spec
//! section: 28.5
//! title: a generic owned field instantiated at an owning inline array is rejected
//! rule: every other substitution — `string`, `Type`, a function type, a
//!   copyable value type, and an owning inline array, whose declared form
//!   `owned [N owned]T` is the double-marking error above — is a compile
//!   error at the instantiation site, with a message naming the rejected
//!   type
//! cites: 28.5, 15.1.1
//! expect: reject
//! diagnostic: instantiation
module Conf;

public class Res
  public constructor()
  end constructor;
end Res;

public class Box<T>
  private owned _slot: T;

  public constructor(owned slot: T)
    _slot := slot;
  end constructor;
end Box;

public class Program
  public method Demo()
    using var b := new Box<[2 owned]Res>([new Res(), new Res()]);
  end Demo;
end Program;
