//! case: s21_10_0024
//! doc: spec
//! section: 21.10
//! title: a local record declared in a constructor body is usable there
//! rule: "`LocalTypeDecl` is admitted wherever a `Statement` is: method, constructor, destructor, and accessor bodies"
//! cites: 22.7
//! expect: run
//! output: 10
module Conf;

public class W
  private _v: int;
  public constructor(v: int)
    record R(q: int)
      public method D(): int => q * 2;
    end R;
    _v := new R(v).D();
  end constructor;
  public method Get(): int => _v;
end W;

public class Program
  public static method Main()
    Console.WriteLine(new W(5).Get());
  end Main;
end Program;
