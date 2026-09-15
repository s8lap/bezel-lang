//! case: s21_5_0001
//! doc: spec
//! section: 21.5
//! title: a labeled break exits the named enclosing loop from a nested loop
//! rule: "`break name;` exits the enclosing loop labeled `name`. After
//!   `break name;`, execution continues at the first statement after the
//!   labeled loop."
//! cites: 21.5
//! expect: run
//! stdout: ok
module Conf;

public class Program
  public static method Main()
    var rows: Array<int> := [2, 2, 2];
    scan: for var count in rows do
      for var i in 1..count do
        if i = 2 then
          Console.WriteLine("found bad");
          break scan;
        end if;
        Console.WriteLine("ok");
      end;
    end scan;
    Console.WriteLine("after");
  end Main;
end Program;
