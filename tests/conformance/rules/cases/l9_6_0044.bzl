//! case: l9_6_0044
//! doc: stdlib
//! section: 9.6
//! title: Trim with a string-typed variable as its chars argument is admitted
//! rule: stdlib §9.6 declares `public pure method Trim(chars: IEnumerable<char>): string;`. Language spec §4.6
//!   states that `string` implements `IEnumerable<char>`, so a string-typed variable is an admitted chars argument.
//! cites: 9.6, 4.6
//! expect: run
//! stdout: 0
//! issue: 7492
//! notes: the variable form of l9_6_0043. The result is the empty string because the receiver is empty.
module Conf;

public class Program
  public static method Main()
    var chars := "xy";
    Console.WriteLine("".Trim(chars).Length);
  end Main;
end Program;
