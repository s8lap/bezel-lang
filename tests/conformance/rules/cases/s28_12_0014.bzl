//! case: s28_12_0014
//! doc: spec
//! section: 28.12
//! title: a destructor body's call of an instance method of this runs on a live instance
//! rule: An instance is **logically disposed** — the state §28.2's
//!   checked-mode test faults on — once its logical disposal has completed;
//!   until then the disposal's own accesses (a destructor body's, the
//!   `DisposeAsync` body's and its chained `base.DisposeAsync()`'s) run on a
//!   live instance.
//! cites: 28.12, 28.2, 28.4
//! expect: run
//! output: closing res
//!   done
module Conf;

public class Res
  private _name: string := "res";

  public method Describe(): string => $"closing {_name}";

  destructor
    Console.WriteLine(this.Describe());
  end destructor;
end Res;

public class Program
  public static method Main()
    do
      using var r := new Res();
    end;
    Console.WriteLine("done");
  end Main;
end Program;
