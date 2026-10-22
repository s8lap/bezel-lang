//! case: l3_2_0006
//! doc: stdlib
//! section: 3.2
//! title: each standard encoding reports its Name through parameters, fields, array elements and conditionals
//! rule: Stdlib §3.2: "`UTF8` (`Name` = `"utf-8"`)", "`UTF16` (`Name` = `"utf-16le"`)", "`UTF32` (`Name` = `"utf-32le"`)",
//!   "`ASCII` (`Name` = `"us-ascii"`)". The singletons are `Encoding`-typed values (the concrete subclasses are not
//!   public API), so `Name` reads the same through a parameter, a class field, an array element and a conditional.
//! cites: 3.2
//! expect: run
//! output: utf-8
//!   us-ascii
//!   utf-16le
//!   utf-32le
//!   utf-16le
//!   utf-8
module Conf;

public class Holder
  public Enc: Encoding;
  public constructor(e: Encoding) this.Enc := e; end constructor;
end Holder;

public class Program
  static method Pn(e: Encoding): string return e.Name; end Pn;

  public static method Main()
    var e: Encoding := Encoding.UTF8;
    Console.WriteLine(e.Name);
    Console.WriteLine(Pn(Encoding.ASCII));
    var e2: Encoding := Encoding.UTF16;
    Console.WriteLine(Pn(e2));
    var h := new Holder(Encoding.UTF32);
    Console.WriteLine(h.Enc.Name);
    var arr := new Array<Encoding>(2, Encoding.UTF16);
    Console.WriteLine(arr[0].Name);
    var t := true;
    var c: Encoding := (if t then Encoding.UTF8 else Encoding.ASCII);
    Console.WriteLine(c.Name);
  end Main;
end Program;
