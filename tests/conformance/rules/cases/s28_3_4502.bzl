//! case: s28_3_4502
//! doc: spec
//! section: 28.3
//! title: a chain of moves between interface-typed locals ends in one disposal of the
//!   value
//! rule: An owned local is disposed exactly once on every control-flow path.
//! cites: 28.8.1
//! expect: run
//! output: mydisp
//!   mydisp
//!   end
//! issue: 7960
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

public class K
  public static method Main(): int
    var i := 0;
    while i < 2 do
      var e: IEnumerator<int> := new MyE();
      var m := move e;
      var m2 := move m;
      using var m3 := move m2;
      i := i + 1;
    end;
    Console.WriteLine("end");
    return 0;
  end Main;
end K;
