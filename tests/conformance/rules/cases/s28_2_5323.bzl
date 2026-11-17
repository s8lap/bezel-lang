//! case: s28_2_5323
//! doc: spec
//! section: 28.2
//! title: a SortedSet<int> disposed through an owned IDisposable? field is not released again at teardown
//! rule: `Dispose()` on any class instance is idempotent: the first call performs the disposal;
//!   subsequent calls are no-ops.
//! cites: 28.2, 28.4, 28.6
//! expect: run
//! output: 200
//! flags: --checked-disposal
module Conf;

public class Holder
  public owned F: IDisposable? := null;

  public constructor() end constructor;

  public method Close()
    this.F?.Dispose();
  end Close;
end Holder;

public class Program
  public static method Main()
    var total := 0;
    for var i in 0..<200 do
      var c := new SortedSet<int>();
      c.Add(i);
      total := total + c.Count;
      using var h := new Holder();
      h.F := c;
      h.Close();
    end;
    Console.WriteLine(total);
  end Main;
end Program;
