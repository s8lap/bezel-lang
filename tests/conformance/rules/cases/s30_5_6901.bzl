//! case: s30_5_6901
//! doc: spec
//! section: 30.5
//! title: a function value read from a Task<Object> result stays callable after the task is
//!   disposed
//! rule: Values of runtime-managed type (`string`, `Type`, boxed value types, function
//!   values) and of ordinary value type carry no classification and no obligation
//! cites: 30.5, 28.9, 30.3
//! expect: run
//! stdout: 3875
//! notes: Task<Object> is Pattern A (30.5): the task may be awaited from multiple places
//!   and every awaiter receives a borrow of the same result; a function value in the result
//!   is runtime-managed, so disposing the task disposes nothing of it.
module Conf;
type Fn := () -> int;
public class Program
  public static async method F(k: int): Task<Object>
    var f := lambda() => k + 1;
    var b: Object := f as Object;
    return b;
  end F;
  public static async method Slow(k: int): Task<Object>
    var f := lambda() => k + 2;
    var b: Object := f as Object;
    await Task.Delay(1);
    return b;
  end Slow;
  public static async method Fwd(k: int): Task<Object>
    return await F(k);
  end Fwd;
  public static async method Main(): Task<int>
    var fs := new Array<Fn>();
    for var i in 0..<50 do
      do
        using var t := F(i);
        var r := await t;
        var r2 := await t;
        var g := r2 as Fn;
        fs.Add(g!);
      end;
      do
        using var t := Slow(i);
        var r := await t;
        var g := r as Fn;
        fs.Add(g!);
      end;
      do
        using var t := Fwd(i);
        var r := await t;
        var g := r as Fn;
        fs.Add(g!);
      end;
    end for;
    var n := 0;
    for var f in fs do n := n + f(); end for;
    Console.WriteLine(n);
    return 0;
  end Main;
end Program;
