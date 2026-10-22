//! case: s16_6_0010
//! doc: spec
//! section: 16.6
//! title: a reversed range pattern with constant shift-expression ulong bounds warns that it
//!   never matches
//! rule: "Because a *constant* reversed range pattern can never match, the compiler emits a
//!   **warning** at the pattern ('this pattern never matches — bounds are reversed')"
//!   (§16.6, v0.340). The bounds `1UL << 63` and `1UL << 62` are constant expressions
//!   (§17.14) and the lower exceeds the upper.
//! cites: 17.14
//! expect: warn
//! diagnostic: never matches
module Conf;

public class Program
  public static method Main()
    var u: ulong := 3UL;
    case u of
      (1UL << 63)..(1UL << 62) => Console.WriteLine("matched"),
      _ => Console.WriteLine("fallback")
    end case;
  end Main;
end Program;
