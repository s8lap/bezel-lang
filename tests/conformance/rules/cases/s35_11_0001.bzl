//! case: s35_11_0001
//! doc: spec
//! section: 35.11
//! title: a generic choice's classification is evaluated per instantiation
//! rule: Classification is evaluated per instantiation: a choice with Ok(owned value:
//!   T) is value-like at T = int (marker erased, payload an admissible value) and
//!   owning at T = Connection (marker means owned).
//! cites: 35.11, 35.4, 28.1
//! expect: run
//! output: 4
//!   4
module Conf;

public choice Outcome<T>
  Ok(owned value: T),
  Empty;
end Outcome;

public class Program
  public static method Main()
    var o := Outcome<int>.Ok(4);    // value-like at T = int: the `owned` marker is erased
    var p := o;                     // so this copies — an owning instantiation would move
    case o of
      Ok(var v) => Console.WriteLine(v),
      Empty     => Console.WriteLine("empty")
    end case;
    case p of
      Ok(var v) => Console.WriteLine(v),
      Empty     => Console.WriteLine("empty")
    end case;
  end Main;
end Program;
