//! case: s18_5_0904
//! doc: spec
//! section: 18.5
//! title: a ToString that throws in an interpolation hole after an await is rethrown at the await
//! rule: A hole *without* a format spec formats via the value's ordinary `ToString()`
//! cites: 18.5, 8.5
//! expect: run
//! output: caught=30
module Conf;

public class T
  public constructor()
  end constructor;

  public override method ToString(): string
    throw new InvalidOperationException("render");
  end ToString;
end T;

public class Program
  public static async method Render(): Task<string>
    using var t := new T();
    await Task.Delay(0);
    return $"{t}";
  end Render;

  public static async method Main(): Task
    var caught := 0;
    var i := 0;
    while i < 30 do
      try
        var s := await Render();
        Console.WriteLine(s);
      catch
        InvalidOperationException => do caught := caught + 1; end,
      end try;
      i := i + 1;
    end while;
    Console.WriteLine("caught=" + caught.ToString());
  end Main;
end Program;
