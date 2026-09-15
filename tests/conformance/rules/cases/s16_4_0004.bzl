//! case: s16_4_0004
//! doc: spec
//! section: 16.4
//! title: step is not legal in case patterns
//! rule: "`step` is not legal in `case` patterns (patterns test the written
//!   bounds; stride has no meaning)." Confirmed by the grammar: `RangePattern ::=
//!   PatternBound (".." | "..<") PatternBound` carries no step clause (Appendix A).
//! cites: 16.4, 19.3
//! expect: reject
//! diagnostic: not legal
module Conf;

public class Program
  public static method Main()
    var delta: int := 5;
    case delta of
      0..10 step 2 => Console.WriteLine("hit"),
      _            => Console.WriteLine("miss")
    end case;
  end Main;
end Program;
