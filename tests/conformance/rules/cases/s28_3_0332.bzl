//! case: s28_3_0332
//! doc: spec
//! section: 28.3
//! title: a Pop of an unbound Stack temporary used within its full expression is admitted
//! rule: A temporary is an admissible borrow source only within its boundary; a borrow or frame
//!   that would escape it is **rejected, never lifetime-extended**
//! cites: 28.3, 4.11
//! expect: run
//! stdout: 3
module Conf;

public class Cell
  public v: int := 3;
  public method Self(): Cell => this;
end Cell;

public class Mk
  public static method S(c: Cell): owned Stack<Cell>
    var s := new Stack<Cell>(); s.Push(c); return s;
  end S;
  public static method Q(c: Cell): owned Queue<Cell>
    var s := new Queue<Cell>(); s.Enqueue(c); return s;
  end Q;
  public static method A(c: Cell): owned Array<Cell>
    var s := new Array<Cell>(); s.Add(c); return s;
  end A;
  public static method D(c: Cell): owned Dictionary<int, Cell>
    var s := new Dictionary<int, Cell>(); s.Add(1, c); return s;
  end D;
  public static method OS(): owned OwningStack<Cell>
    var s := new OwningStack<Cell>(); s.Push(new Cell()); return s;
  end OS;
end Mk;

public class Program
  public static method Main()
    using var c := new Cell();
    Console.WriteLine(Mk.S(c).Pop().v);
  end Main;
end Program;
