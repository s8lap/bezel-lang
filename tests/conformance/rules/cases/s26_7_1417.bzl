//! case: s26_7_1417
//! doc: spec
//! section: 26.7
//! title: an extension over a record receiver may store a copy of the receiver into a class field
//! rule: a store landing in the receiver's own storage is a compile error (a store beyond a class- or
//!   interface-typed hop leaves the receiver and is legal
//! cites: 26.7, 9.4
//! expect: run
//! stdout: 3
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

public extension Pt
  public method M1(h: Holder): int
    h.s := this;
    return this.X;
  end M1;
end extension;

public class Program
  public static method Main()
    using var h := new Holder();
    var p := new Pt(3);
    Console.WriteLine(p.M1(h));
  end Main;
end Program;
