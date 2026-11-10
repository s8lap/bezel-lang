//! case: s28_3_0932
//! doc: spec
//! section: 28.3
//! title: an if-expression mixing a named owned Task local with a fresh Task is rejected
//! rule: The **meet** of the arms' ownership states, identically to the `a ?? b` row above
//! cites: 30.3, 20
//! expect: reject
//! diagnostic: meet
//! issue: 7807
module Conf;
public class Program
  static method Go(flag: bool)
    using var t: Task? := Task.Delay(0);
    var u := (if flag then t! else Task.Delay(0));
  end Go;

  public static method Main()
    Console.WriteLine(1);
  end Main;
end Program;
