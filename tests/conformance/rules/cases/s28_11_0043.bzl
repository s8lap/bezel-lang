//! case: s28_11_0043
//! doc: spec
//! section: 28.11
//! title: move of a slot of a plain inline-array field is rejected
//! rule: A copy, shared-reference or borrow slot, and a read-only view, admit none
//! cites: 28.11, 15.1, 28.8.1
//! expect: reject
//! diagnostic: slot
module Conf;

public class Res
  public Name: string;
  public constructor(n: string)
    this.Name := n;
  end constructor;
end Res;

public class Holder
  public Slots: [2]Res?;
end Holder;

public class Program
  public static method Main(): int
    using var h := new Holder();
    using var t := move h.Slots[0];
    return 0;
  end Main;
end Program;
