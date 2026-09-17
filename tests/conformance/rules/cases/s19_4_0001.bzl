//! case: s19_4_0001
//! doc: spec
//! section: 19.4
//! title: arms are tried in order and the first true guard wins
//! rule: Arms are evaluated in textual order. The first arm whose pattern
//!   matches and whose guard is true is selected.
//! cites: 19.4
//! expect: run
//! output: Hot: 35
module Conf;

public class Program
  public static method Main()
    var temperature := 35;
    case temperature of
      var t when t > 30 => Console.WriteLine($"Hot: {t}");,
      var t when t < 0  => Console.WriteLine($"Freezing: {t}");,
      var t             => Console.WriteLine($"Mild: {t}");
    end case;
  end Main;
end Program;
