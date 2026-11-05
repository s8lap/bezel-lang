//! case: s26_7_1413
//! doc: spec
//! section: 26.7
//! title: an extension over Random that stores nothing from its receiver is admitted
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

public extension Random
  public method M1(h: Holder): int
    h.o := null;
    return 1;
  end M1;
end extension;

public class Program
  public static method Main()
    using var h := new Holder();
    using var r := new Random(1);
    Console.WriteLine(r.M1(h));
  end Main;
end Program;
