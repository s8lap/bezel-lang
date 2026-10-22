//! case: s16_6_0009
//! doc: spec
//! section: 16.6
//! title: a reversed range pattern with named ulong constant bounds warns that it never matches
//! rule: "Because a *constant* reversed range pattern can never match, the compiler emits a
//!   **warning** at the pattern ('this pattern never matches — bounds are reversed')"
//!   (§16.6, v0.340). `(Big)..(Five)` is reversed for `ulong`: the lower bound
//!   18446744073709551615 exceeds the upper bound 5.
//! cites: 19.3
//! expect: warn
//! diagnostic: never matches
module Conf;

public class Program
  public const Big: ulong := 18446744073709551615UL;
  public const Five: ulong := 5UL;

  public static method Main()
    var u: ulong := 3UL;
    case u of
      (Big)..(Five) => Console.WriteLine("matched"),
      _ => Console.WriteLine("fallback")
    end case;
  end Main;
end Program;
