//! case: l5_9953
//! doc: stdlib
//! section: 5
//! title: every predicate terminal over an empty Dictionary raises or answers null
//! rule: `FirstOrNull(pred)`/`LastOrNull(pred)`/`SingleOrNull(pred)` return `null` when none matches
//! cites: 5, 4.8
//! expect: run
//! output: FON none
//!   LON none
//!   SON none
//!   First raised
//!   Last raised
//!   Single raised
//! notes: Pins #9261. An empty Dictionary has no matching element for any predicate: the OrNull forms
//!   answer null; First, Last and Single raise InvalidOperationException (stdlib 5, 'raise ... when
//!   no element matches').
module Conf;
import System;
public class Program
  public static method Main()
    using var d := new Dictionary<string, int>();
    var fo := d.FirstOrNull(lambda(e) => e.Value > 5);
    if fo = null then Console.WriteLine("FON none"); else Console.WriteLine("FON found"); end;
    var lo := d.LastOrNull(lambda(e) => e.Value > 5);
    if lo = null then Console.WriteLine("LON none"); else Console.WriteLine("LON found"); end;
    var so := d.SingleOrNull(lambda(e) => e.Value > 5);
    if so = null then Console.WriteLine("SON none"); else Console.WriteLine("SON found"); end;
    try
      var x := d.First(lambda(e) => true);
      Console.WriteLine("First returned");
    catch ex: InvalidOperationException =>
      Console.WriteLine("First raised");
    end;
    try
      var y := d.Last(lambda(e) => true);
      Console.WriteLine("Last returned");
    catch ex: InvalidOperationException =>
      Console.WriteLine("Last raised");
    end;
    try
      var z := d.Single(lambda(e) => true);
      Console.WriteLine("Single returned");
    catch ex: InvalidOperationException =>
      Console.WriteLine("Single raised");
    end;
  end Main;
end Program;
