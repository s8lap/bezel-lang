//! case: s28_11_0045
//! doc: spec
//! section: 28.11
//! title: move of a slot of a plain inline-array call result is rejected
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
  static method Pair(x: Res): [2]Res?
    return [x, null];
  end Pair;

  public static method Main(): int
    using var x := new Res("x");
    using var t := move Pair(x)[0];
    return 0;
  end Main;
end Program;
