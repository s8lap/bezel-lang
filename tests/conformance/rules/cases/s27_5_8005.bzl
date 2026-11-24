//! case: s27_5_8005
//! doc: spec
//! section: 27.5
//! title: an async nested lambda assigns a function local of an async lambda and the
//!   awaiting body reads it
//! rule: Modifications to a captured variable inside the lambda are visible outside, and
//!   vice versa.
//! cites: 27.5, 30.6
//! expect: run
//! stdout: 20100
//! notes: Pins the #3743 regression fix with an async nested assigner (the base aborted with
//!   a closure over-release).
module Conf;
public class C
  public static async method Run2(n: int): Task<int>
    var o2 := async lambda(): Task<int> => do
      var f := lambda(): int => n;
      var mk := async lambda(): Task<int> => do
        f := lambda(): int => n + 1;
        await Task.Delay(1);
        return 0;
      end;
      var t := await mk();
      return t + f();
    end;
    return await o2();
  end Run2;
  public static async method Main(): Task
    var total := 0;
    for var j in 0..<200 do total := total + await Run2(j); end;
    Console.WriteLine(total);
  end Main;
end C;
