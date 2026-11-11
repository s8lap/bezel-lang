//! case: s28_3_4505
//! doc: spec
//! section: 28.3
//! title: an interface-typed owned argument moved into a local inside the callee is
//!   disposed once there
//! rule: An owned local is disposed exactly once on every control-flow path.
//! cites: 22.3, 28.8.1
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
  public static method Sink(owned x: IEnumerator<int>)
    var y := move x;
  end Sink;
  public static method Main(): int
    K.Sink(new MyE());
    K.Sink(new MyE());
    Console.WriteLine("end");
    return 0;
  end Main;
end K;
