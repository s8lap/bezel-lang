//! case: s3_6_0053
//! doc: spec
//! section: 3.6
//! title: two constrained type parameters each read their own constraint's members
//! rule: "A generic body type-checks under the weakest assumption its constraints establish" ([§3.6]). With `where T: Exception` and `where U: ArgumentException`, `x.Message` and `y.Message` each read through their own constraint. `ParamName` is "`string?`" ([§31.4]) and `??` supplies `"-"` only when it is null, so the supplied name `pp` is used.
//! cites: 31.4
//! expect: run
//! output: abpp
module Conf;
public class Program
  public static method M<T, U>(x: T, y: U): string where T: Exception where U: ArgumentException
    return x.Message + y.Message + (y.ParamName ?? "-");
  end M;
  public static method Main()
    using var e := new Exception("a");
    using var a := new ArgumentException("b", "pp");
    Console.WriteLine(M(e, a));
  end Main;
end Program;
