//! case: s28_3_10_4705
//! doc: spec
//! section: 28.3.10
//! title: r?.Dispose() on one branch and auto-drop on the other dispose a nullable local once across an await
//! rule: including through the null-conditional and null-forgiving spellings `x?.Dispose()` /
//!   `x!.Dispose()`, required for nullable owned locals
//! cites: 28.3.10, 30.1
//! expect: run
//! stdout: 100 133 0
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
    var r: Res? := (if k mod 3 = 0 then null else new Res("n"));
    await Task.Delay(0);
    if k mod 2 = 0 then
      r?.Dispose();
    end if;
    return k mod 2;
  end Worker;
  public static async method Main(): Task
    var n := 0;
    var s := 0;
    while n < 200 do
      s := s + await Program.Worker(n);
      n := n + 1;
    end while;
    Console.WriteLine($"{s} {Res.made} {Res.live}");
  end Main;
end Program;
