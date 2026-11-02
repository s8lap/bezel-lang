//! case: s5_1_0171
//! doc: spec
//! section: 5.1
//! title: an un-narrowed nullable argument does not convert to a non-nullable parameter
//! rule: **Nullable conversions.** For any type `T`, `T` implicitly converts to `T?`: for a reference type, the same reference viewed as nullable ([§7.1](#sec-non-nullable-and-nullable-types)); for a value type, the present-value wrapping ([§7.2](#sec-nullable-value-types)). Each implicit conversion of this section also **lifts** to the nullable forms: `S?` implicitly converts to `T?` whenever `S` implicitly converts to `T` — a present value converts by the underlying conversion, `null` converts to `null` (`int?` → `long?`); constant narrowing (above) participates for a present constant value.
//! cites: 5.1, 7.4, 7.1
//! expect: reject
//! diagnostic: nullable
//! issue: 7957
module Conf;
import System.Serialization;

public class Program
  public static method Main(): int
    var o := new JsonOptions(true, Make(), false, false, 3);
    Console.WriteLine(o.IndentWidth);
    return 0;
  end Main;

  public static method Make(): int? => 2;
end Program;
