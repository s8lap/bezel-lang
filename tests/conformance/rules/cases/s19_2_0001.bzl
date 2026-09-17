//! case: s19_2_0001
//! doc: spec
//! section: 19.2
//! title: return inside an arm exits the enclosing callable, not the arm
//! rule: `return` inside an arm exits the enclosing callable, not the arm.
//!   `case` cannot be exited "early" via `return`; restructure into helper
//!   methods.
//! cites: 19.2
//! expect: run
//! stdout: skip 1
module Conf;

public class Program
  public static method FindFirstEven(nums: Array<int>): string
    for var n in nums do
      case n mod 2 of
        0 => return $"found {n}";,
        _ => Console.WriteLine($"skip {n}");
      end case;
    end;
    return "none";
  end FindFirstEven;

  public static method Main()
    var nums: Array<int> := [1, 3, 4, 5];
    Console.WriteLine(FindFirstEven(nums));
  end Main;
end Program;
