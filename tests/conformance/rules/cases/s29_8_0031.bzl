//! case: s29_8_0031
//! doc: spec
//! section: 29.8
//! title: the one enumerator disposes a T-typed parameter at an owning-choice instantiation once
//! rule: The first `GetEnumerator()` (or `GetAsyncEnumerator()`) transfers the
//!   frame's `owned` and move-only parameters to the enumerator it produces;
//!   that enumerator disposes any such parameter the body has not moved out of
//!   when it completes or is disposed
//! cites: 29.8, 15.3, 35.4
//! expect: run
//! output: 1
//!   closed
//!   after
//! issue: 5902
module Conf;

public class Conn
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;
end Conn;

public choice Slot
  Held(owned c: Conn),
  Empty;
end Slot;

public class Program
  public static method Each<T>(x: T): owned IEnumerable<int>
    yield return 1;
  end Each;

  public static method Main()
    for var a in Each<Slot>(Slot.Held(new Conn())) do
      Console.WriteLine(a);
    end;
    Console.WriteLine("after");
  end Main;
end Program;
