//! case: s21_10_0022
//! doc: spec
//! section: 21.10
//! title: two same-named local records in different methods are distinct types
//! rule: "two same-shaped local records in different methods are distinct types"
//! cites: 9
//! expect: run
//! output: 3
//!   151
module Conf;

public class A
  public static method F(): int
    var q := 1;
    record Row(a: int)
      public method Get(): int => a + 1;
    end Row;
    return new Row(1).Get() + q;
  end F;
end A;

public class B
  public static method F(): int
    var a := 50;
    record Row(b: int)
      public method Get(): int => b + 100;
    end Row;
    return new Row(1).Get() + a;
  end F;
end B;

public class Program
  public static method Main()
    Console.WriteLine(A.F());
    Console.WriteLine(B.F());
  end Main;
end Program;
