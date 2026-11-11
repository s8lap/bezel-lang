//! case: s28_3_10_4703
//! doc: spec
//! section: 28.3.10
//! title: a plain var owned local moved on every path after an await is skipped by the scope-end disposal
//! rule: so the scope-end disposal skips it; nothing is disposed twice
//! cites: 30.1, 28.8.1
//! expect: run
//! output: 300
//!   400
//!   0
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
  public static method Sink(owned p: Res): int
    var n := p.Name.Length;
    p.Dispose();
    return n;
  end Sink;

  public static async method Worker(k: int): Task<int>
    var r := new Res("m");
    var keep := new Res("k");
    await Task.Delay(0);
    if k > 0 then
      return Program.Sink(move r);
    end if;
    await Task.Delay(0);
    var v := Program.Sink(move r);
    return v + keep.Name.Length;
  end Worker;
  public static async method Main(): Task
    var n := 0;
    var s := 0;
    while n < 200 do
      s := s + await Program.Worker(n mod 2);
      n := n + 1;
    end while;
    Console.WriteLine(s);
    Console.WriteLine(Res.made);
    Console.WriteLine(Res.live);
  end Main;
end Program;
