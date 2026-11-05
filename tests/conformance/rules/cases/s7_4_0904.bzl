//! case: s7_4_0904
//! doc: spec
//! section: 7.4
//! title: a ReadOnlyStringSpan? local reassigned from a Slice call stays narrowed in the loop
//! rule: assignment: after `x := expr;` where `expr`'s static type is non-nullable
//! cites: 7.4
//! expect: run
//! output: abcdefgh
//!   6
//!   bcdefg
//!   4
//!   cdef
//!   2
//!   de
//!   0
module Conf;

public class Program
  public static method Main()
    var w := "abcdefgh";
    var s: ReadOnlyStringSpan? := new ReadOnlyStringSpan(w, 0, 8);
    var i := 0;
    while i < 4 do
      if s <> null then
        Console.WriteLine(s.ToString());
        if s.Length <= 1 then
          s := null;
        else
          s := s.Slice(1, s.Length - 2);
          Console.WriteLine(s.Length);
        end if;
      end if;
      i := i + 1;
    end while;
  end Main;
end Program;
