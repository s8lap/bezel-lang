//! case: s7_5_0031
//! doc: spec
//! section: 7.5
//! title: ?. over string instance methods yields the member's result for a non-null string and null otherwise
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 7.5, 9.6
//! expect: run
//! issue: 7747
//! output: nn ABCDE|bc|**abcde|True|2
//!   nl null|null|null|False|-1
module Conf;

public class Program
  static method Show(label: string, s: string?)
    var u := s?.ToUpper() ?? "null";
    var sub := s?.Substring(1, 2) ?? "null";
    var pad := s?.PadLeft(7, '*') ?? "null";
    var has := s?.Contains("c") ?? false;
    var idx := s?.IndexOf("c") ?? -1;
    Console.WriteLine($"{label} {u}|{sub}|{pad}|{has}|{idx}");
  end Show;

  public static method Main()
    Show("nn", "abcde");
    Show("nl", null);
  end Main;
end Program;
