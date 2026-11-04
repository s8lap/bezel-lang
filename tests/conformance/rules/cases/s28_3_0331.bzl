//! case: s28_3_0331
//! doc: spec
//! section: 28.3
//! title: returning the Pop of an unbound Stack temporary from a block-bodied method is rejected
//! rule: A temporary is an admissible borrow source only within its boundary; a borrow or frame
//!   that would escape it is **rejected, never lifetime-extended**
//! cites: 28.3, 4.11
//! expect: reject
//! diagnostic: temporar
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

public class Probe
  public method Get(c: Cell): Cell
    return Mk.S(c).Pop();
  end Get;
end Probe;

public class Program
  public static method Main()
    using var c := new Cell();
    using var p := new Probe();
    Console.WriteLine(p.Get(c).v);
  end Main;
end Program;
