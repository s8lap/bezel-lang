//! case: s30_5_0033
//! doc: spec
//! section: 30.5
//! title: an unbound Pattern A producer read within one expression is disposed at that expression's end
//! rule: An **unbound owned temporary** (§28.3) — a bare producer call or other
//!   unnamed owned task expression — bounds the borrow by its K8
//!   full-expression boundary, *provided the borrowed result is used only
//!   within that expression*: the temporary is disposed at the expression's
//!   end, after every use of the borrow, and disposing the task disposes the
//!   borrowed result with it.
//! cites: 30.5, 28.3
//! expect: run
//! output: dispose q
//!   got q
//! notes: the initializer reads a string member, so it is not exactly the
//!   await and is no scoped await-bind; the task and its result die at the
//!   declaration's end, before the next statement prints.
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
    var s := (await Producer.MakeAsync("q")).Name();
    Console.WriteLine($"got {s}");
  end Main;
end Program;
