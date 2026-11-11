//! case: s30_5_4701
//! doc: spec
//! section: 30.5
//! title: a bare var x := await of an OwningTask is an owned local that auto-drops at scope exit
//! rule: an `x` never discharged auto-drops at scope exit with the must-use warning.
//! cites: 28.3.10, 30.1
//! expect: run
//! stdout: 600 400 0
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
  public static async method Make(n: string): OwningTask<Res>
    await Task.Delay(0);
    return new Res(n);
  end Make;

  public static async method Worker(): Task<int>
    var x := await Program.Make("b");
    await Task.Delay(0);
    var y := await Program.Make("cc");
    return x.Name.Length + y.Name.Length;
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
