//! case: s22_5_0024
//! doc: spec
//! section: 22.5
//! title: Named arguments of a stdlib constructor evaluate in written order (Semaphore, OwningBuffer)
//! rule: A call's supplied argument expressions are evaluated in call-site textual order, left to right as
//!   written, for positional and named arguments alike; naming an argument changes which parameter it
//!   binds to, never when it runs. Binding by name covers
//!   `Semaphore(initialCount, maxCount)` and `OwningBuffer<T>(length, init)` in any written order.
//! cites: 22.5, 22.6
//! expect: run
//! output: MIL
//!   18450
module Conf;

import System.Threading;

public class Program
  public static method T(sb: StringBuilder, s: string, v: int): int
    sb.Append(s);
    return v;
  end T;

  public static method Main()
    var total := 0;
    for var i in 0..<100 do
      using var sb := new StringBuilder();
      using var a := new Semaphore(maxCount := T(sb, "M", 5), initialCount := T(sb, "I", 2));
      using var b := new Semaphore(2, 5);
      using var c := new Semaphore(2, maxCount := 5);
      using var d := new Semaphore(initialCount := 2, maxCount := 5);
      using var ob1 := new OwningBuffer<int>(init := lambda(k) => k * 10 + i, length := T(sb, "L", 3));
      using var ob2 := new OwningBuffer<int>(3, lambda(k) => k * 10 + i);
      using var ob3 := new OwningBuffer<int>(length := 3, init := lambda(k) => k * 10 + i);
      using var ob4 := new OwningBuffer<int>(3);
      using var ob5 := new OwningBuffer<int>(length := 3);
      total := total + ob1[1] + ob2[1] + ob3[1] + ob4.Length + ob5.Length;
      if i = 0 then
        Console.WriteLine(sb.ToString());
      end;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
