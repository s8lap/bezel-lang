//! case: l5_0046
//! doc: stdlib
//! section: 5
//! title: Contains, Any and All with string elements resolve on list interfaces
//! rule: Contains(value: T) is declared where T: hashable and string is hashable; Any(predicate) and All(predicate) are section 5 quantifiers; sa is ["ab", "cde"].
//! cites: 5
//! expect: run
//! output: True
//!   False
//!   True
//!   False
module Conf;

public class Program
  public static method Id(x: long): long => x;
  public static method Pick(l: IList<long>): long => l.Sum() + l.Max() + l.First();

  public static method Main()
    var a: Array<int> := [5, 3, 8, 1];
    var l: IList<int> := a;
    var r: IReadOnlyList<int> := a;
    var la: Array<long> := [5000000000L, 3L, 8L];
    var ll: IList<long> := la;
    var lr: IReadOnlyList<long> := la;
    var da: Array<double> := [1.5, 2.5];
    var dl: IList<double> := da;
    var dr: IReadOnlyList<double> := da;
    var sa: Array<string> := ["ab", "cde"];
    var sl: IList<string> := sa;
    var sr: IReadOnlyList<string> := sa;
    Console.WriteLine(sl.Contains("ab"));
    Console.WriteLine(sr.Contains("zz"));
    Console.WriteLine(sl.Any(lambda(s) => s.Length > 2));
    Console.WriteLine(sr.All(lambda(s) => s.Length > 2));
  end Main;
end Program;
