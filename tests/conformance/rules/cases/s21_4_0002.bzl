//! case: s21_4_0002
//! doc: spec
//! section: 21.4
//! title: for over a range iterates its integer values in order
//! rule: "`for` iterates any iterable source... and `Range<T>` values
//!   (which the compiler may optimize into counted loops)."
//! cites: 21.4
//! expect: run
//! output: 1
//!   2
//!   3
module Conf;

public class Program
  public static method Main()
    for var i in 1..3 do
      Console.WriteLine(i.ToString());
    end;
  end Main;
end Program;
