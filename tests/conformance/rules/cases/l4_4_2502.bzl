//! case: l4_4_2502
//! doc: stdlib
//! section: 4.4
//! title: an untyped Sorted comparer lambda over a string array sorts like the typed one
//! rule: public method Sorted(comparer: (T, T) -> int): owned Array<T>;  // fresh sorted copy
//! cites: 27.4
//! expect: run
//! output: True True
module Conf;
public class Program
  public static method Main()
    var s: Array<string> := ["bb", "a", "ccc"];
    using var typed := s.Sorted(lambda(x: string, y: string): int => x.Length - y.Length);
    using var untyped := s.Sorted(lambda(x, y) => x.Length - y.Length);
    var same := true;
    for var i in 0..<3 do
      if typed[i] <> untyped[i] then
        same := false;
      end if;
    end for;
    var moved := typed[0] <> "bb";
    Console.WriteLine($"{same} {moved}");
  end Main;
end Program;
