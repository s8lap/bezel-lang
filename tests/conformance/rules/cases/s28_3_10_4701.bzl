//! case: s28_3_10_4701
//! doc: spec
//! section: 28.3.10
//! title: a plain var owned local held across an await is auto-dropped when a loop breaks
//! rule: On a path that never moves it out, a plain `var` owned local (or undischarged `owned` parameter)
//!   thus behaves like a `using var`: reclaimed when its scope ends.
//! cites: 30.1, 28.2
//! expect: run
//! stdout: 600 800 0
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
  public static async method Worker(): Task<int>
    var i := 0;
    while true do
      var r := new Res($"a{i}");
      await Task.Delay(0);
      if i > 2 then
        break;
      end if;
      i := i + 1;
    end while;
    return i;
  end Worker;
  public static async method Main(): Task
    var n := 0;
    var s := 0;
    while n < 200 do
      s := s + await Program.Worker();
      n := n + 1;
    end while;
    Console.WriteLine($"{s} {Res.made} {Res.live}");
  end Main;
end Program;
