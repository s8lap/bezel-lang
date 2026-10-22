//! case: s21_10_0020
//! doc: spec
//! section: 21.10
//! title: a local record names an earlier local record inside its own method body
//! rule: "Within its scope the name is an ordinary type reference in"
//! cites: 9
//! expect: run
//! output: 9
module Conf;

public class Program
  public static method Main()
    record A(x: int)
      public method D(): int => x * 2;
    end A;
    record B(y: int)
      public method Via(): int
        var a := new A(y);
        return a.D() + 1;
      end Via;
    end B;
    Console.WriteLine(new B(4).Via());
  end Main;
end Program;
