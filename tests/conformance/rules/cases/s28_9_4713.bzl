//! case: s28_9_4713
//! doc: spec
//! section: 28.9
//! title: a string aliased by an Object local across loop reassignments is read intact
//! rule: a `using var`, a discard or scope end over such a value disposes nothing; the type's
//!   lifetime is managed by the runtime without user involvement
//! cites: 28.9, 8.6, 4.6
//! expect: run
//! stdout: 2760
//! issue: 8598
module Conf;

public class Cell
  public V: int := 0;
end Cell;

public class Holder
  public owned C: Cell := new Cell();

  public method Peek(o: Object): int => o.ToString().Length;

  public method M(x: string): int
    var o: Object := this.C;
    var n := 0;
    var j := 0;
    while j < 3 do
      o := x;
      n := n + this.Peek(o);
      o := this.C;
      o := x;
      j := j + 1;
    end;
    return n + this.Peek(o);
  end M;
end Holder;

public class Make
  public static method Str(n: int): string => "s" + n.ToString();
end Make;

public class Program
  public static method Main()
    var i := 0;
    var total := 0;
    while i < 200 do
      var h := new Holder();
      do
        var x := Make.Str(i);
        total := total + h.M(x);
      end;
      i := i + 1;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
