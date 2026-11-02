//! case: s28_11_0048
//! doc: spec
//! section: 28.11
//! title: move of a slot of a static readonly plain inline array is rejected
//! rule: A copy, shared-reference or borrow slot, and a read-only view, admit none
//! cites: 28.11, 15.1, 22.12
//! expect: reject
//! diagnostic: slot
module Conf;

public class Res
  public Name: string;
  public constructor(n: string)
    this.Name := n;
  end constructor;
end Res;

public class Registry
  public static readonly Pool: [2]Res?;
end Registry;

public class Program
  public static method Main(): int
    using var t := move Registry.Pool[0];
    return 0;
  end Main;
end Program;
