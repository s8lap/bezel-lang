//! case: s30_2_0005
//! doc: spec
//! section: 30.2
//! title: a case-arm binding is read after an await inside the arm of an async method
//! rule: "`await` suspends the current async method until a task completes" (§30.2, v0.340); the
//!   arm's `var n` binding (§19.3) is read again after the first `await` resumes, so the
//!   method returns the concatenation `7` then `8`.
//! cites: 19.3, 30.1
//! expect: run
//! output: 78
//!   z
module Conf;

public class Program
  public static async method F(k: int): Task<string>
    var r := "";
    case k of
      var n when n > 5 => do
        await Task.Delay(1);
        r := r + n.ToString();
        await Task.Delay(1);
        r := r + (n + 1).ToString();
      end,
      _ => r := "z";
    end;
    return r;
  end F;

  public static async method Main(): Task<int>
    Console.WriteLine(await F(7));
    Console.WriteLine(await F(1));
    return 0;
  end Main;
end Program;
