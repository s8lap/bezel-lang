//! case: s30_5_0031
//! doc: spec
//! section: 30.5
//! title: scoped await-binds dispose in reverse declaration order, interleaved with a using var
//! rule: In particular: the slot's position on the cleanup stack is the
//!   declaration's, so a `using var` declared *after* the bind disposes
//!   *before* the slot and two scoped binds in one scope are disposed in
//!   reverse order.
//! cites: 30.5, 28.10
//! expect: run
//! issue: 3048
//! output: a c b
//!   dispose b
//!   dispose c
//!   dispose a
//!   end
//! notes: each Res is the kind-1 result of its task, so a hidden slot's
//!   disposal is observed as the result's destructor; c is a hand-written
//!   using var declared between the two binds.
module Conf;

public class Res
  private _n: string;
  public constructor(n: string) _n := n; end constructor;

  public method Name(): string => _n;

  destructor
    Console.WriteLine($"dispose {_n}");
  end destructor;
end Res;

public class Producer
  public static async method MakeAsync(n: string): Task<Res>
    await Task.Delay(0);
    return new Res(n);
  end MakeAsync;
end Producer;

public class Program
  public static async method Main(): Task
    do
      var a := await Producer.MakeAsync("a");
      using var c := new Res("c");
      var b := await Producer.MakeAsync("b");
      Console.WriteLine($"{a.Name()} {c.Name()} {b.Name()}");
    end do;
    Console.WriteLine("end");
  end Main;
end Program;
