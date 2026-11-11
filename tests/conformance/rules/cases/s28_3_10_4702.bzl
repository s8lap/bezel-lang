//! case: s28_3_10_4702
//! doc: spec
//! section: 28.3.10
//! title: a plain var owned local held across an await is disposed on the throwing path too
//! rule: the same implicit disposal that the *Exception-path discharge* below performs on the unwinding
//!   edge, now applied to the normal-exit edge as well
//! cites: 30.1, 31.2
//! expect: run
//! stdout: 100 100 200 0
module Conf;

public class Res
  public static live: int := 0;
  public static made: int := 0;
  private _name: string;
  public constructor(name: string)
    _name := name;
    Res.live := Res.live + 1;
    Res.made := Res.made + 1;
  end constructor;
  public property Name: string => _name;
  destructor
    Res.live := Res.live - 1;
  end destructor;
end Res;

public class Program
  public static async method Worker(k: int): Task<int>
    var r := new Res("w");
    await Task.Delay(0);
    if k mod 2 = 0 then
      throw new InvalidOperationException("boom");
    end if;
    return r.Name.Length;
  end Worker;

  public static async method Main(): Task
    var n := 0;
    var s := 0;
    var c := 0;
    while n < 200 do
      try
        s := s + await Program.Worker(n);
      catch
        InvalidOperationException => do c := c + 1; end
      end try;
      n := n + 1;
    end while;
    Console.WriteLine($"{s} {c} {Res.made} {Res.live}");
  end Main;
end Program;
