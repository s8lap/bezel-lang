//! case: l2_3_0026
//! doc: stdlib
//! section: 2.3
//! title: Convert.ToString of narrowed nullable long and int with base arguments
//! rule: stdlib §2.3 ToString(value, toBase) returns the text value.ToString(toBase) returns: for a negative value "a `-` for a negative value followed by the magnitude's digits" (§9.1), lower-case. -255 base 16 is -ff, 5 base 2 is 101. §7.4 (v0.350): narrowing is flow-sensitive and "a null test ... `x <> null` ... narrowing the true branch"; a narrowed nullable local has the non-nullable type, so a stdlib parameter of the underlying type accepts it.
//! cites: 2.3, 9.1, 7.4
//! expect: run
//! output: -ff
//!   101
module Conf;

public class Program
  public static method IdL(x: long?): long? return x; end IdL;
  public static method IdI(x: int?): int? return x; end IdI;
  public static method Main()
    var l: long? := IdL(-255L);
    var b: int? := IdI(2);
    var i: int? := IdI(5);
    if l <> null and b <> null and i <> null then
      Console.WriteLine(Convert.ToString(l, 16));
      Console.WriteLine(Convert.ToString(i, b));
    end;
  end Main;
end Program;
