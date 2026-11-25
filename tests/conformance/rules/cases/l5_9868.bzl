//! case: l5_9868
//! doc: stdlib
//! section: 5
//! title: Max with a selector over an empty source raises InvalidOperationException at double and string
//! rule: InvalidOperationException on an empty source (§5's
//! cites: 5, 14.2
//! expect: run
//! output: ioe
//!   ioe-s
//! notes: Harvested from the #8689 review (z_Max_empty). Stdlib 5 states the empty-source rule for
//!   "Min/Max/Average, selector and no-selector forms alike". Here the selector form is reached
//!   through a generic body at T := double and at T := string, both over an empty Array. Each call
//!   raises InvalidOperationException before anything is printed, so the output is the two catch
//!   markers.
module Conf;

public class Program
  static method H<T>(a: Array<T>): T where T: IComparable<T> => a.Max(lambda(x) => x);

  public static method Main()
    var a := new Array<double>();
    var s := new Array<string>();
    try
      Console.WriteLine(H(a));
    catch InvalidOperationException =>
      Console.WriteLine("ioe");
    end;
    try
      Console.WriteLine(H(s));
    catch InvalidOperationException =>
      Console.WriteLine("ioe-s");
    end;
  end Main;
end Program;
