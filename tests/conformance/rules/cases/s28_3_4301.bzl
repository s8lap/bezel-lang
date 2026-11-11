//! case: s28_3_4301
//! doc: spec
//! section: 28.3
//! title: an owned IEnumerator parameter holding a user class instance is disposed at the end of the callee
//! rule: it is disposed implicitly at that scope end (for an `owned` parameter, at the end of the method)
//! cites: 28.3.10, 28.1, 28.2
//! expect: run
//! output: take
//!   mydisp
//!   take
//!   mydisp
//!   end
//! issue: 7961
module Conf;

class MyE: IEnumerator<int>
  private i: int := 0;

  public method MoveNext(): bool
    if this.i < 2 then
      this.i := this.i + 1;
      return true;
    end if;
    return false;
  end MoveNext;

  public property Current: int => this.i;

  destructor
    Console.WriteLine("mydisp");
  end destructor;
end MyE;

class Y
  public static method Take(owned e: IEnumerator<int>)
    Console.WriteLine("take");
  end Take;
end Y;

public class Program
  public static method Main()
    Y.Take(new MyE());
    Y.Take(new MyE());
    Console.WriteLine("end");
  end Main;
end Program;
