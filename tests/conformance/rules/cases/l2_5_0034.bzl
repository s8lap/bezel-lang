//! case: l2_5_0034
//! doc: stdlib
//! section: 2.5
//! title: TimeSpan / double: zero divisor, NaN, range and tiny-divisor edges
//! rule: stdlib §2.5 (TimeSpan `operator /`): "same computation model and OverflowException rule as *; DivideByZeroException on divisor = 0.0". The divisor 0.0 or -0.0 (equal to 0.0) raises DivideByZeroException; a NaN divisor or a quotient outside the tick range raises OverflowException; a quotient that is an in-range double is converted to ticks.
//! cites: 2.5
//! expect: run
//! output: max/-1.0: -9223372036854775808
//!   min/1.0: -9223372036854775808
//!   min/-1.0: OVF
//!   100/0.0: DIV0
//!   100/-0.0: DIV0
//!   0/0.0: DIV0
//!   min/0.0: DIV0
//!   100/NaN: OVF
//!   100/Inf: 0
//!   100/tiny: OVF
//!   0/tiny: 0
//!   100/3.0: 33
//!   -100/3.0: -33
//!   2^62/0.5: OVF
//!   max/NaN: OVF
//!   2^62/(0.5+ulp): 9223372036854773760
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
      var r := mx / -1.0;
      Console.WriteLine($"max/-1.0: {r.Ticks}");
    catch OverflowException => Console.WriteLine("max/-1.0: OVF");
    catch DivideByZeroException => Console.WriteLine("max/-1.0: DIV0");
    end;
    try
      var r := mn / 1.0;
      Console.WriteLine($"min/1.0: {r.Ticks}");
    catch OverflowException => Console.WriteLine("min/1.0: OVF");
    catch DivideByZeroException => Console.WriteLine("min/1.0: DIV0");
    end;
    try
      var r := mn / -1.0;
      Console.WriteLine($"min/-1.0: {r.Ticks}");
    catch OverflowException => Console.WriteLine("min/-1.0: OVF");
    catch DivideByZeroException => Console.WriteLine("min/-1.0: DIV0");
    end;
    try
      var r := t100 / 0.0;
      Console.WriteLine($"100/0.0: {r.Ticks}");
    catch OverflowException => Console.WriteLine("100/0.0: OVF");
    catch DivideByZeroException => Console.WriteLine("100/0.0: DIV0");
    end;
    try
      var r := t100 / nz;
      Console.WriteLine($"100/-0.0: {r.Ticks}");
    catch OverflowException => Console.WriteLine("100/-0.0: OVF");
    catch DivideByZeroException => Console.WriteLine("100/-0.0: DIV0");
    end;
    try
      var r := tz / 0.0;
      Console.WriteLine($"0/0.0: {r.Ticks}");
    catch OverflowException => Console.WriteLine("0/0.0: OVF");
    catch DivideByZeroException => Console.WriteLine("0/0.0: DIV0");
    end;
    try
      var r := mn / 0.0;
      Console.WriteLine($"min/0.0: {r.Ticks}");
    catch OverflowException => Console.WriteLine("min/0.0: OVF");
    catch DivideByZeroException => Console.WriteLine("min/0.0: DIV0");
    end;
    try
      var r := t100 / nan;
      Console.WriteLine($"100/NaN: {r.Ticks}");
    catch OverflowException => Console.WriteLine("100/NaN: OVF");
    catch DivideByZeroException => Console.WriteLine("100/NaN: DIV0");
    end;
    try
      var r := t100 / inf;
      Console.WriteLine($"100/Inf: {r.Ticks}");
    catch OverflowException => Console.WriteLine("100/Inf: OVF");
    catch DivideByZeroException => Console.WriteLine("100/Inf: DIV0");
    end;
    try
      var r := t100 / 1e-300;
      Console.WriteLine($"100/tiny: {r.Ticks}");
    catch OverflowException => Console.WriteLine("100/tiny: OVF");
    catch DivideByZeroException => Console.WriteLine("100/tiny: DIV0");
    end;
    try
      var r := tz / 1e-300;
      Console.WriteLine($"0/tiny: {r.Ticks}");
    catch OverflowException => Console.WriteLine("0/tiny: OVF");
    catch DivideByZeroException => Console.WriteLine("0/tiny: DIV0");
    end;
    try
      var r := t100 / 3.0;
      Console.WriteLine($"100/3.0: {r.Ticks}");
    catch OverflowException => Console.WriteLine("100/3.0: OVF");
    catch DivideByZeroException => Console.WriteLine("100/3.0: DIV0");
    end;
    try
      var r := TimeSpan.FromTicks(-100L) / 3.0;
      Console.WriteLine($"-100/3.0: {r.Ticks}");
    catch OverflowException => Console.WriteLine("-100/3.0: OVF");
    catch DivideByZeroException => Console.WriteLine("-100/3.0: DIV0");
    end;
    try
      var r := p62 / 0.5;
      Console.WriteLine($"2^62/0.5: {r.Ticks}");
    catch OverflowException => Console.WriteLine("2^62/0.5: OVF");
    catch DivideByZeroException => Console.WriteLine("2^62/0.5: DIV0");
    end;
    try
      var r := mx / nan;
      Console.WriteLine($"max/NaN: {r.Ticks}");
    catch OverflowException => Console.WriteLine("max/NaN: OVF");
    catch DivideByZeroException => Console.WriteLine("max/NaN: DIV0");
    end;
    try
      var r := p62 / 0.5000000000000001;
      Console.WriteLine($"2^62/(0.5+ulp): {r.Ticks}");
    catch OverflowException => Console.WriteLine("2^62/(0.5+ulp): OVF");
    catch DivideByZeroException => Console.WriteLine("2^62/(0.5+ulp): DIV0");
    end;
  end Main;
end Program;
