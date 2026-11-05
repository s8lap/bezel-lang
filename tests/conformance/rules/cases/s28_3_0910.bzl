//! case: s28_3_0910
//! doc: spec
//! section: 28.3
//! title: a lambda in an instance method may return its own plain Array parameter, in both body forms
//! rule: Lambda into such a slot:* `null` (nullable slot), a capture-sourced borrow, a
//!   plain-parameter-sourced borrow, or static storage
//! cites: 27.3
//! expect: run
//! output: 4
//!   5
module Conf;

public class Holder
  public constructor()
  end constructor;

  public method Run()
    using var a := new Array<int>();
    a.Add(4);
    a.Add(5);
    var f: (Array<int>) -> Array<int> := lambda(x) => x;
    var g: (Array<int>) -> Array<int> := lambda(x: Array<int>) => do
      return x;
    end;
    Console.WriteLine(f(a)[0]);
    Console.WriteLine(g(a)[1]);
  end Run;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    h.Run();
  end Main;
end Program;
