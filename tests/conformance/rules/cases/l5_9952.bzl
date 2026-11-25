//! case: l5_9952
//! doc: stdlib
//! section: 5
//! title: First, Last and Single with a predicate over a Dictionary raise on no match; Single also on
//!   several
//! rule: `First(pred)`/`Last(pred)`/`Single(pred)` raise `InvalidOperationException` when no element
//!   matches (and `Single(pred)` also when more than one matches)
//! cites: 5, 4.8
//! expect: run
//! output: First raised
//!   Last raised
//!   Single raised
//!   Single many raised
//! notes: Pins #9261. Entries a=1, b=9, c=12; no entry has Value > 100, so First, Last and Single raise
//!   InvalidOperationException; Value > 0 matches all three entries, so Single raises again (more
//!   than one match). Each handler prints its marker; a call that returns would print a different
//!   line.
module Conf;
import System;
public class Program
  public static method Main()
    using var d := new Dictionary<string, int>();
    d["a"] := 1;
    d["b"] := 9;
    d["c"] := 12;
    try
      var x := d.First(lambda(e) => e.Value > 100);
      Console.WriteLine("First returned");
    catch ex: InvalidOperationException =>
      Console.WriteLine("First raised");
    end;
    try
      var y := d.Last(lambda(e) => e.Value > 100);
      Console.WriteLine("Last returned");
    catch ex: InvalidOperationException =>
      Console.WriteLine("Last raised");
    end;
    try
      var z := d.Single(lambda(e) => e.Value > 100);
      Console.WriteLine("Single returned");
    catch ex: InvalidOperationException =>
      Console.WriteLine("Single raised");
    end;
    try
      var w := d.Single(lambda(e) => e.Value > 0);
      Console.WriteLine("Single many returned");
    catch ex: InvalidOperationException =>
      Console.WriteLine("Single many raised");
    end;
  end Main;
end Program;
