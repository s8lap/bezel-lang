//! case: s28_3_4506
//! doc: spec
//! section: 28.3
//! title: interface-typed owned locals live at an exception are each disposed once on
//!   the unwinding edge
//! rule: Owned locals and owned parameters that are live when an exception propagates
//!   out of their enclosing block are disposed implicitly during stack unwinding
//! cites: 28.8.1
//! expect: run
//! output: mydisp
//!   mydisp
//!   caught
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
  public static method Thr()
    var e: IEnumerator<int> := new MyE();
    using var m := move e;
    var e2: IEnumerator<int> := new MyE();
    var m2 := move e2;
    throw new InvalidOperationException("x");
  end Thr;
  public static method Main(): int
    try
      K.Thr();
    catch
      InvalidOperationException => Console.WriteLine("caught"),
    end try;
    Console.WriteLine("end");
    return 0;
  end Main;
end K;
