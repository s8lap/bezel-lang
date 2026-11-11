//! case: s28_3_4501
//! doc: spec
//! section: 28.3
//! title: an interface-typed owned local moved into a using var is disposed exactly
//!   once per iteration
//! rule: An owned local is disposed exactly once on every control-flow path.
//! cites: 28.8.1, 28.10
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
      using var m := move e;
      i := i + 1;
    end;
    Console.WriteLine("end");
    return 0;
  end Main;
end K;
