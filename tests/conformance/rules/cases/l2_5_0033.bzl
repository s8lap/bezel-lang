//! case: l2_5_0033
//! doc: stdlib
//! section: 2.5
//! title: TimeSpan * double: range, NaN and double-precision edges
//! rule: stdlib §2.5 (TimeSpan `operator *`): "computes over Ticks in double; OverflowException on a product outside the tick range or a NaN operand (pinned)"; the tick range is long.MinValue..long.MaxValue. Each row's product is formed from the tick count converted to double: an out-of-range product, an infinite product and a NaN operand raise OverflowException; in-range products (including exactly long.MinValue) return the tick count; 2^53+1 ticks times 1.0 loses the low bit in double.
//! cites: 2.5
//! expect: run
//! output: min*1.0: -9223372036854775808
//!   max*2.0: OVF
//!   min*2.0: OVF
//!   max*-1.0: -9223372036854775808
//!   min*-1.0: OVF
//!   max*0.5: 4611686018427387904
//!   min*0.5: -4611686018427387904
//!   max*NaN: OVF
//!   100*NaN: OVF
//!   100*Inf: OVF
//!   100*-Inf: OVF
//!   100*0.0: 0
//!   100*-0.0: 0
//!   2^62*2: OVF
//!   -2^62*2: -9223372036854775808
//!   2^62*1.9999: 9222910868252933120
//!   2^53+1*1.0: 9007199254740992
//!   max*0.9999999999999999: 9223372036854774784
module Conf;

public class Program
  public static method Main()
    var mx := TimeSpan.MaxValue;
    var mn := TimeSpan.MinValue;
    var tz := TimeSpan.Zero;
    var t100 := TimeSpan.FromTicks(100L);
    var p62 := TimeSpan.FromTicks(4611686018427387904L);
    var m62 := TimeSpan.FromTicks(-4611686018427387904L);
    var t53 := TimeSpan.FromTicks(9007199254740993L);
    var z := 0.0;
    var nan := z / z;
    var inf := 1.0 / z;
    var ninf := -1.0 / z;
    var nz := -z;
    try
      var r := mn * 1.0;
      Console.WriteLine($"min*1.0: {r.Ticks}");
    catch OverflowException => Console.WriteLine("min*1.0: OVF");
    catch DivideByZeroException => Console.WriteLine("min*1.0: DIV0");
    end;
    try
      var r := mx * 2.0;
      Console.WriteLine($"max*2.0: {r.Ticks}");
    catch OverflowException => Console.WriteLine("max*2.0: OVF");
    catch DivideByZeroException => Console.WriteLine("max*2.0: DIV0");
    end;
    try
      var r := mn * 2.0;
      Console.WriteLine($"min*2.0: {r.Ticks}");
    catch OverflowException => Console.WriteLine("min*2.0: OVF");
    catch DivideByZeroException => Console.WriteLine("min*2.0: DIV0");
    end;
    try
      var r := mx * -1.0;
      Console.WriteLine($"max*-1.0: {r.Ticks}");
    catch OverflowException => Console.WriteLine("max*-1.0: OVF");
    catch DivideByZeroException => Console.WriteLine("max*-1.0: DIV0");
    end;
    try
      var r := mn * -1.0;
      Console.WriteLine($"min*-1.0: {r.Ticks}");
    catch OverflowException => Console.WriteLine("min*-1.0: OVF");
    catch DivideByZeroException => Console.WriteLine("min*-1.0: DIV0");
    end;
    try
      var r := mx * 0.5;
      Console.WriteLine($"max*0.5: {r.Ticks}");
    catch OverflowException => Console.WriteLine("max*0.5: OVF");
    catch DivideByZeroException => Console.WriteLine("max*0.5: DIV0");
    end;
    try
      var r := mn * 0.5;
      Console.WriteLine($"min*0.5: {r.Ticks}");
    catch OverflowException => Console.WriteLine("min*0.5: OVF");
    catch DivideByZeroException => Console.WriteLine("min*0.5: DIV0");
    end;
    try
      var r := mx * nan;
      Console.WriteLine($"max*NaN: {r.Ticks}");
    catch OverflowException => Console.WriteLine("max*NaN: OVF");
    catch DivideByZeroException => Console.WriteLine("max*NaN: DIV0");
    end;
    try
      var r := t100 * nan;
      Console.WriteLine($"100*NaN: {r.Ticks}");
    catch OverflowException => Console.WriteLine("100*NaN: OVF");
    catch DivideByZeroException => Console.WriteLine("100*NaN: DIV0");
    end;
    try
      var r := t100 * inf;
      Console.WriteLine($"100*Inf: {r.Ticks}");
    catch OverflowException => Console.WriteLine("100*Inf: OVF");
    catch DivideByZeroException => Console.WriteLine("100*Inf: DIV0");
    end;
    try
      var r := t100 * ninf;
      Console.WriteLine($"100*-Inf: {r.Ticks}");
    catch OverflowException => Console.WriteLine("100*-Inf: OVF");
    catch DivideByZeroException => Console.WriteLine("100*-Inf: DIV0");
    end;
    try
      var r := t100 * 0.0;
      Console.WriteLine($"100*0.0: {r.Ticks}");
    catch OverflowException => Console.WriteLine("100*0.0: OVF");
    catch DivideByZeroException => Console.WriteLine("100*0.0: DIV0");
    end;
    try
      var r := t100 * nz;
      Console.WriteLine($"100*-0.0: {r.Ticks}");
    catch OverflowException => Console.WriteLine("100*-0.0: OVF");
    catch DivideByZeroException => Console.WriteLine("100*-0.0: DIV0");
    end;
    try
      var r := p62 * 2.0;
      Console.WriteLine($"2^62*2: {r.Ticks}");
    catch OverflowException => Console.WriteLine("2^62*2: OVF");
    catch DivideByZeroException => Console.WriteLine("2^62*2: DIV0");
    end;
    try
      var r := m62 * 2.0;
      Console.WriteLine($"-2^62*2: {r.Ticks}");
    catch OverflowException => Console.WriteLine("-2^62*2: OVF");
    catch DivideByZeroException => Console.WriteLine("-2^62*2: DIV0");
    end;
    try
      var r := p62 * 1.9999;
      Console.WriteLine($"2^62*1.9999: {r.Ticks}");
    catch OverflowException => Console.WriteLine("2^62*1.9999: OVF");
    catch DivideByZeroException => Console.WriteLine("2^62*1.9999: DIV0");
    end;
    try
      var r := t53 * 1.0;
      Console.WriteLine($"2^53+1*1.0: {r.Ticks}");
    catch OverflowException => Console.WriteLine("2^53+1*1.0: OVF");
    catch DivideByZeroException => Console.WriteLine("2^53+1*1.0: DIV0");
    end;
    try
      var r := mx * 0.9999999999999999;
      Console.WriteLine($"max*0.9999999999999999: {r.Ticks}");
    catch OverflowException => Console.WriteLine("max*0.9999999999999999: OVF");
    catch DivideByZeroException => Console.WriteLine("max*0.9999999999999999: DIV0");
    end;
  end Main;
end Program;
