//! case: s21_5_0003
//! doc: spec
//! section: 21.5
//! title: a labeled loop closes with its label or a bare end
//! rule: A labeled loop's `end` may repeat the loop label (`end scan;`),
//!   and bare `end;` remains valid everywhere.
//! cites: 21.5, 1.6
//! expect: run
//! stdout: 1
module Conf;

public class Program
  public static method Main()
    scan: for var i in 1..3 do
      Console.WriteLine(i.ToString());
    end scan;
  end Main;
end Program;
