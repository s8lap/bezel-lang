//! case: s5_1_6711
//! doc: spec
//! section: 5.1
//! title: a type parameter converts to the interface its constraint names
//! rule: **a type parameter to an interface or class its constraints guarantee
//! cites: 14.2
//! expect: run
//! stdout: 7
module Conf;

public interface IMark
  method Id(): int;
end IMark;

public class A: IMark
  public method Id(): int => 7;
end A;

public class Program
  static method Show<T>(x: T): int where T: IMark
    var m: IMark := x;
    return m.Id();
  end Show;
  public static method Main()
    using var a := new A();
    Console.WriteLine(Program.Show(a));
  end Main;
end Program;
