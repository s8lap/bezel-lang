//! case: s22_5_0058
//! doc: spec
//! section: 22.5
//! title: named arguments of an extension method on a fresh receiver run in written order
//! rule: naming an argument changes which parameter it binds to, never when it runs
//! cites: 22.5
//! expect: run
//! output: eval b
//!   eval a
//!   abr
module Conf;

extension StringBuilder
  public method Join(a: string, b: string): string => a + b + this.ToString();
end;

public class Program
  public static method Tag(s: string): string
    Console.WriteLine("eval " + s);
    return s;
  end Tag;

  public static method Main()
    Console.WriteLine(new StringBuilder("r").Join(b := Tag("b"), a := Tag("a")));
  end Main;
end Program;
