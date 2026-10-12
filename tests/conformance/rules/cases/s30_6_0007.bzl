//! case: s30_6_0007
//! doc: spec
//! section: 30.6
//! title: the closed target return slot, not the body alone, picks Task<C> over OwningTask<C>
//! rule: (2) Otherwise a **closed target return slot** names it ... a fresh
//!   owned class result is admitted under either `Task<C>` (Pattern A, kind 1
//!   — the task owns the result) or `OwningTask<C>` (Pattern B), so the target
//!   decides between them. ... a closed head binds nothing, so reading it is
//!   reading a declared type, not an inference.
//! cites: 30.6, 30.5
//! expect: run
//! output: True
//!   True
//!   res freed
//!   res freed
module Conf;

public class Res
  destructor
    Console.WriteLine("res freed");
  end destructor;
end Res;

public class Holder
  public static method AsTask(): Task<Res>
    var f: (int) -> Task<Res> := async lambda(x) => do
      return new Res();
    end;
    return f(1);
  end AsTask;

  public static method AsOwning(): OwningTask<Res>
    var g: (int) -> OwningTask<Res> := async lambda(x) => do
      return new Res();
    end;
    return g(1);
  end AsOwning;
end Holder;

public class Program
  public static method Main()
    using var t := Holder.AsTask();
    Console.WriteLine(t.IsCompleted);
    using var u := Holder.AsOwning();
    Console.WriteLine(u.IsCompleted);
  end Main;
end Program;
