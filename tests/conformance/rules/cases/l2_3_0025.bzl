//! case: l2_3_0025
//! doc: stdlib
//! section: 2.3
//! title: Convert.ToString of a narrowed long? renders decimal, and base 16 in lower case
//! rule: stdlib §2.3: Convert.ToString(value: long) and ToString(value: long, toBase: int); the radix form "emits the same text as ToString(radix)" which emits "the lower-case digits" (§9.1). §7.4 (v0.350): narrowing is flow-sensitive and "a null test ... `x <> null` ... narrowing the true branch"; a narrowed nullable local has the non-nullable type, so a stdlib parameter of the underlying type accepts it. long.MaxValue renders 9223372036854775807 and 7fffffffffffffff.
//! cites: 2.3, 9.1, 7.4
//! expect: run
//! output: 9223372036854775807
//!   7fffffffffffffff
module Conf;

public class Program
  public static method IdL(x: long?): long? return x; end IdL;
  public static method Main()
    var l: long? := IdL(9223372036854775807L);
    if l <> null then
      Console.WriteLine(Convert.ToString(l));
      Console.WriteLine(Convert.ToString(l, 16));
    end;
  end Main;
end Program;
