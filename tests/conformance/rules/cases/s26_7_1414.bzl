//! case: s26_7_1414
//! doc: spec
//! section: 26.7
//! title: an extension over Array<int> that stores nothing from its receiver is admitted
//! rule: The receiver is always a borrow; extensions cannot store `this`, return it as `owned`, or
//!   dispose it.
//! cites: 26.7
//! expect: run
//! stdout: 1
module Conf;

public class Holder
  public o: Object?;

  public constructor()
    o := null;
  end constructor;
end Holder;

public extension Array<int>
  public method M1(h: Holder): int
    h.o := null;
    return 1;
  end M1;
end extension;

public class Program
  public static method Main()
    using var h := new Holder();
    using var a := new Array<int>();
    Console.WriteLine(a.M1(h));
  end Main;
end Program;
