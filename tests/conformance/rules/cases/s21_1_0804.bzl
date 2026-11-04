//! case: s21_1_0804
//! doc: spec
//! section: 21.1
//! title: a local initialized from the built-in type of the same name (var Guid := Guid.NewGuid()) works
//! rule: The declaration's own initializer is not in that scope
//! cites: 21.1, 2.3
//! expect: run
//! output: True
//!   36
//!   True
//!   90
//!   True
//!   True
//! issue: 8019
//! notes: control: instance use of a local that shares its built-in type's name; Guid renders 36 characters in the D form (stdlib 2.6).
module Conf;

import System.Diagnostics;

public class Program
  public static method Main()
    var Random := new Random(42);
    var r := Random.Next(5);
    Console.WriteLine(r < 5);
    var Guid := Guid.NewGuid();
    Console.WriteLine(Guid.ToString().Length);
    var DateTime := DateTime.Now;
    Console.WriteLine(DateTime.Year > 2000);
    var TimeSpan := TimeSpan.FromSeconds(90.0);
    Console.WriteLine(TimeSpan.TotalSeconds);
    var Stopwatch := Stopwatch.StartNew();
    Stopwatch.Stop();
    Console.WriteLine(Stopwatch.ElapsedMilliseconds >= 0L);
    var Path := "a/b";
    Console.WriteLine(Path.Contains("/"));
  end Main;
end Program;
