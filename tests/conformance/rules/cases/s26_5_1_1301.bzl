//! case: s26_5_1_1301
//! doc: spec
//! section: 26.5.1
//! title: stdlib Sum over INumber<T> is filtered at a notnull-bounded T, so the Object extension answers
//! rule: A `where` clause is an *applicability* filter, never a ranking signal.
//! cites: 26.5, 14.2
//! expect: run
//! output: 4040
//!   11
module Conf;

extension Object
  public method Sum(): int => 4040;
end;

public class Program
  static method G<T>(a: Array<T>): int where T: notnull => a.Sum();

  static method H<T>(a: Array<T>): owned T where T: INumber<T> => a.Sum();

  public static method Main()
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    Console.WriteLine(G(a));
    Console.WriteLine(H(a));
  end Main;
end Program;
