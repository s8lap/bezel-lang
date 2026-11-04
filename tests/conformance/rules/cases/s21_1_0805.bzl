//! case: s21_1_0805
//! doc: spec
//! section: 21.1
//! title: a local's initializer that mentions Console sees the built-in Console; later uses see the local
//! rule: The declaration's own initializer is not in that scope
//! cites: 21.1, 2.7
//! expect: run
//! output: init
//!   USER:x
//! issue: 8126
module Conf;

public class Sh
  public last: string := "";
  public method WriteLine(s: string)
    this.last := "USER:" + s;
  end WriteLine;
  public method ReadLine(): string
    return "UREAD";
  end ReadLine;
end Sh;

public class Program
  public static method MkSh(f: () -> ()): owned Sh
    f();
    return new Sh();
  end MkSh;
  public static method Show(s: string)
    Console.WriteLine(s);
  end Show;
  public static method Main()
    var Console := MkSh(lambda() => Console.WriteLine("init"));
    Console.WriteLine("x");
    Show(Console.last);
  end Main;
end Program;
