//! case: s28_11_0051
//! doc: spec
//! section: 28.11
//! title: move of a slot of a generic owning inline-array field is admitted
//! rule: a slot of either owning inline-array form, `[N owned]T?` or `[N owned]T`
//! cites: 28.11, 15.1.1, 28.8.1, 14.2
//! expect: accept
module Conf;

public class Res
  public Name: string;
  public constructor(n: string)
    this.Name := n;
  end constructor;
end Res;

public class Box<T> where T: class
  public Slots: [2 owned]T?;

  public method Take(): owned T?
    return move this.Slots[0];
  end Take;
end Box;

public class Program
  public static method Use()
    using var b := new Box<Res>();
    using var t := b.Take();
  end Use;
end Program;
