//! case: s28_11_0044
//! doc: spec
//! section: 28.11
//! title: move of a slot of a plain inline-array parameter is rejected
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

public class Program
  static method Take(a: [2]Res?): owned Res?
    return move a[0];
  end Take;

  public static method Main(): int
    return 0;
  end Main;
end Program;
