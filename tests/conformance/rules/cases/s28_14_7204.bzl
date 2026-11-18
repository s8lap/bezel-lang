//! case: s28_14_7204
//! doc: spec
//! section: 28.14
//! title: a constructor storing its plain function-typed parameter into
//!   another object's field is rejected
//! rule: It may not be stored in a field of any object
//! cites: 28.14, 22.7
//! expect: reject
//! diagnostic: stored
module Conf;

public class Holder
  public h: () -> int := lambda(): int => 0;
end Holder;

public class K
  public constructor(f: () -> int, o: Holder)
    o.h := f;
  end constructor;
end K;

public class Program
  public static method Main()
    using var o := new Holder();
    do
      var s := "hi" + "x";
      var k := new K(lambda(): int => s.Length, o);
    end;
    Console.WriteLine(o.h());
  end Main;
end Program;
