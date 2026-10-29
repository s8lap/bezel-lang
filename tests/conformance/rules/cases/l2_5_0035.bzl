//! case: l2_5_0035
//! doc: stdlib
//! section: 2.5
//! title: Repeated TimeSpan * double and / double throws and results stay consistent over 100 rounds
//! rule: stdlib §2.5 (TimeSpan `operator *`): "computes over Ticks in double; OverflowException on a product outside the tick range or a NaN operand (pinned)"; the tick range is long.MinValue..long.MaxValue. stdlib §2.5 (TimeSpan `operator /`): "same computation model and OverflowException rule as *; DivideByZeroException on divisor = 0.0". Per round: Max*2.0 -> Overflow, 100/0.0 -> DivideByZero, 100*NaN -> Overflow, 100*3.0 -> 300 (no throw). 100 rounds: ov=200 dz=100 ok=100.
//! cites: 2.5
//! expect: run
//! stdout: ov=200 dz=100 ok=100
module Conf;

public class Program
  public static method Main()
    var mx := TimeSpan.MaxValue;
    var t100 := TimeSpan.FromTicks(100L);
    var z := 0.0;
    var nan := z / z;
    var ov := 0;
    var dz := 0;
    var ok := 0;
    var i := 0;
    while i < 100 do
      try
        var r := mx * 2.0;
        ok := ok + 1;
      catch OverflowException => ov := ov + 1;
      end;
      try
        var r := t100 / z;
        ok := ok + 1;
      catch DivideByZeroException => dz := dz + 1;
      end;
      try
        var r := t100 * nan;
        ok := ok + 1;
      catch OverflowException => ov := ov + 1;
      end;
      try
        var r := t100 * 3.0;
        ok := ok + 1;
      catch Exception => ov := ov + 1000;
      end;
      i := i + 1;
    end;
    Console.WriteLine($"ov={ov} dz={dz} ok={ok}");
  end Main;
end Program;
