//! case: s16_3_6460
//! doc: spec
//! section: 16.3
//! title: a uint step variable steps a uint range inside an async method body
//! rule: `Step` is independent of `T`'s range, so a step beyond that range is
//!   **accepted at construction**
//! cites: 16.3, 30.1
//! expect: run
//! stdout: [0,3000000000,]
module Conf;
public class Program
  public static async method Rg(st: uint): Task<string>
    var u0: uint := 0;
    var u9: uint := 4000000000;
    var e: IEnumerable<uint> := u0..u9 step st;
    var s := "";
    for var x in e do s := s + $"{x},"; end;
    return "[" + s + "]";
  end Rg;
  public static method Main()
    var st: uint := 3000000000;
    var t := Rg(st);
    Console.WriteLine(t.Result);
  end Main;
end Program;
