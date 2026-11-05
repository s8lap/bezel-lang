//! case: s26_7_1418
//! doc: spec
//! section: 26.7
//! title: an extension over DateTime may store a copy of the receiver into a class field
//! rule: a store landing in the receiver's own storage is a compile error (a store beyond a class- or
//!   interface-typed hop leaves the receiver and is legal
//! cites: 26.7, 9.4
//! expect: run
//! stdout: 1
module Conf;

public record Pt(X: int);

public class Holder
  public s: Pt;
  public d: DateTime;

  public constructor()
    s := new Pt(0);
    d := DateTime.MinValue;
  end constructor;
end Holder;

public extension DateTime
  public method M1(h: Holder): int
    h.d := this;
    return 1;
  end M1;
end extension;

public class Program
  public static method Main()
    using var h := new Holder();
    var dt := DateTime.MinValue;
    Console.WriteLine(dt.M1(h));
  end Main;
end Program;
