//! case: l5_1_1201
//! doc: stdlib
//! section: 5.1
//! title:Min() on a lazy Where over strings yields the smallest string element
//! rule:public method Min(): T;
//! cites: 5
//! expect: run
//! stdout: b
module Conf;

public class Program
  public static method Main()
    var a: Array<string> := ["b", "a", "c"];
    var m := a.Where(lambda(x: string) => x <> "a").Min();
    Console.WriteLine(m);
  end Main;
end Program;
