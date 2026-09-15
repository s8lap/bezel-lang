//! case: s29_3_0001
//! doc: spec
//! section: 29.3
//! title: an iterator method's body drives a for loop over its yielded values
//! rule: `yield return expression;` produces the next value and suspends.
//!   Falling off the end of the method has the same effect as `yield break`.
//! cites: 29.3
//! expect: run
//! stdout: 14
module Conf;

public class Program
  public static method Squares(n: int): IEnumerable<int>
    for var i in 0..<n do
      yield return i * i;
    end;
  end Squares;

  public static method Main()
    var total := 0;
    for var s in Squares(4) do
      total := total + s;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
