//! case: s30_3_0202
//! doc: spec
//! section: 30.3
//! title: a break after a using var over a null-conditional Result still disposes the result
//! rule: reading it on a completed task in an owned-consuming position transfers ownership of the result
//!   to the reader exactly as `await` does
//! cites: 7.5, 28.3
//! expect: run
//! output: 250
//!   live 0
//! issue: 8131
module Conf;

public class Cell
  public static n: int := 0;
  public v: int;

  public constructor(x: int)
    v := x;
    Cell.n := Cell.n + 1;
  end constructor;

  destructor
    Cell.n := Cell.n - 1;
  end destructor;
end Cell;

public class Program
  public static async method Mk(): OwningTask<Cell>
    return new Cell(5);
  end Mk;

  public static method Run()
    var i := 0;
    var s := 0;
    while i < 100 do
      using var t := Mk();
      var o: OwningTask<Cell>? := t;
      using var c := o?.Result;
      s := s + (c?.v ?? 0);
      i := i + 1;
      if i = 50 then
        break;
      end if;
    end while;
    Console.WriteLine(s);
  end Run;

  public static method Main()
    Run();
    Console.WriteLine($"live {Cell.n}");
  end Main;
end Program;
