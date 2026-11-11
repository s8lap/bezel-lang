//! case: s28_9_4701
//! doc: spec
//! section: 28.9
//! title: a plain var local of a trivial-dispose class held across an await draws no diagnostic
//! rule: No **must-use / auto-drop diagnostic** attaches to a trivial-dispose value.
//! cites: 28.3.10, 30.1
//! expect: quiet
module Conf;

public class Cell(k: int)
  public method Get(): int => k;
end Cell;

public class Program
  public static async method Worker(k: int): Task<int>
    var f := new Cell(k);
    await Task.Delay(0);
    var g := new Cell(k + 1);
    await Task.Delay(0);
    return f.Get() + g.Get();
  end Worker;
end Program;
