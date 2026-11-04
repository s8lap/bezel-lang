//! case: s2_7_0803
//! doc: spec
//! section: 2.7
//! title: a lambda parameter named Console, captured by a nested lambda, receives WriteLine
//! rule: If it resolves to a *value* (a local, field, or property), the chain is member access.
//! cites: 2.7, 2.3
//! expect: run
//! stdout: USER:n
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
  public static method Show(s: string)
    Console.WriteLine(s);
  end Show;
  public static method Main()
    var outer := lambda(Console: Sh): string => do
      var inner := lambda(): string => do
        Console.WriteLine("n");
        return Console.last;
      end;
      return inner();
    end;
    var s := new Sh();
    Show(outer(s));
  end Main;
end Program;
