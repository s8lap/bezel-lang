//! case: s21_10_0014
//! doc: spec
//! section: 21.10
//! title: a local record declared inside a record method body is usable within that body
//! rule: "A `record` or `enum` may be declared as a **statement**, scoped to the enclosing body like a local variable"
//! cites: 9
//! expect: run
//! output: 22
module Conf;

public class Program
  public static method Main()
    record Out(a: int)
      public method Get(): int
        record In(b: int)
          public method G(): int => b * 10;
        end In;
        var k := new In(a);
        return k.G() + a;
      end Get;
    end Out;
    Console.WriteLine(new Out(2).Get());
  end Main;
end Program;
