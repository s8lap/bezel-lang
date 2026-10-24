//! case: s5_1_0147
//! doc: spec
//! section: 5.1
//! title: a byte startIndex reaches an int parameter as its unsigned value
//! rule: A value of one type implicitly converts to another when no
//!   programmer-visible information is lost; the conversion table lists `int`
//!   among the implicit targets of `byte`, and `byte` is an UNSIGNED type, so
//!   the widening is a zero-extension and the byte 200 is the int 200 — not the
//!   -56 a sign-extension would give. stdlib §9.6's own contract for the
//!   member reads the bound at that widened value: "code-point index of the
//!   first occurrence at or after startIndex, or -1; 0 <= startIndex <= Length
//!   (IndexOutOfRangeException otherwise)". The receiver here is 220 code
//!   points long, so a startIndex of 200 is inside the bound and the first
//!   '5' at or after it is at 205.
//! cites: 5.1, stdlib 9.6
//! expect: run
//! stdout: 205
module Conf;

public class Program
  public static method Main(): int
    var s := "0123456789";
    var i := 0;
    while i < 21 do
      s := s + "0123456789";
      i := i + 1;
    end while;
    var b: byte := 200;
    Console.WriteLine(s.IndexOf('5', b));
    return 0;
  end Main;
end Program;
