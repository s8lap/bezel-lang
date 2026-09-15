//! case: s21_7_0003
//! doc: spec
//! section: 21.7
//! title: an exhaustive statement-case whose every arm exits grants totality
//! rule: A statement-position `case` counts as exiting on every path — so a
//!   callable may end with it, with no trailing `return` — when the match
//!   is proven statically exhaustive and every arm's body itself exits on
//!   every path.
//! cites: 21.7, 19.6
//! expect: run
//! stdout: halted
module Conf;

public enum Signal
  Go, Stop;
end Signal;

public class Program
  public static method React(s: Signal): string
    case s of
      Go   => return "moving";,
      Stop => return "halted";
    end case;
  end React;

  public static method Main()
    Console.WriteLine(React(Signal.Stop));
  end Main;
end Program;
