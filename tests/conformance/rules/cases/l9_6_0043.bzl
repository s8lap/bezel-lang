//! case: l9_6_0043
//! doc: stdlib
//! section: 9.6
//! title: Trim with a string literal as its chars argument is admitted
//! rule: stdlib §9.6 declares `public pure method Trim(chars: IEnumerable<char>): string;`. Language spec §4.6
//!   states that `string` implements `IEnumerable<char>`, so a string value is an admitted chars argument.
//! cites: 9.6, 4.6
//! expect: run
//! stdout: 0
//! issue: 7492
//! notes: the receiver is the empty string, so the result has no characters whatever the chars set
//!   removes. The case pins admission, not the removal rule, which stdlib §9.6 does not state.
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("".Trim("ab").Length);
  end Main;
end Program;
