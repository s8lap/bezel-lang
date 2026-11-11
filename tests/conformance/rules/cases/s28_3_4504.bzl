//! case: s28_3_4504
//! doc: spec
//! section: 28.3
//! title: an IEnumerable-typed owned local moved to a var and iterated is disposed
//!   once after its loop
//! rule: An owned local is disposed exactly once on every control-flow path.
//! cites: 28.2, 29.7
//! expect: run
//! output: x
//!   x
//!   mydisp
//!   mysdisp
//!   x
//!   x
//!   mydisp
//!   mysdisp
//!   end
//! issue: 8473
module Conf;

public class MyE: IEnumerator<int>
  private i: int := 0;
  public method MoveNext(): bool
    if this.i < 2 then this.i := this.i + 1; return true; end if;
    return false;
  end MoveNext;
  public property Current: int get => this.i; end Current;
  destructor
    Console.WriteLine("mydisp");
  end destructor;
end MyE;

public class MyS: IEnumerable<int>
  public method GetEnumerator(): owned IEnumerator<int>
    return new MyE();
  end GetEnumerator;
  destructor
    Console.WriteLine("mysdisp");
  end destructor;
end MyS;

public class K
  public static method Main(): int
    var i := 0;
    while i < 2 do
      var s: IEnumerable<int> := new MyS();
      var t := move s;
      for var x in t do Console.WriteLine("x"); end;
      i := i + 1;
    end;
    Console.WriteLine("end");
    return 0;
  end Main;
end K;
