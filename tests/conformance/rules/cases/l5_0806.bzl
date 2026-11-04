//! case: l5_0806
//! doc: stdlib
//! section: 5
//! title: no-selector Min on an empty Array of generic records throws InvalidOperationException
//! rule: `Min`/`Max` — selector and no-selector forms alike — and `Average` throw `InvalidOperationException`
//! cites: 5, 5.1
//! expect: run
//! stdout: ioe
//! issue: 8140
module Conf;

public record W<T>(v: int): IComparable<W<T>>
  public method CompareTo(other: W<T>): int => (if v > other.v then 1 else (if v < other.v then -1 else 0));
end W;

public class Program
  public static method Main()
    var e: Array<W<string>> := [];
    try
      var z := e.Min();
      Console.WriteLine("no");
    catch
      InvalidOperationException => Console.WriteLine("ioe"),
    end try;
  end Main;
end Program;
