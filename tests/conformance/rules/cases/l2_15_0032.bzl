//! case: l2_15_0032
//! doc: stdlib
//! section: 2.15
//! title: BeginArray returns -1 and HasMoreElements drives a top-level array read
//! rule: A JSON array; the `elementCount` argument is likewise advisory. On the
//!   read side `JsonDeserializer.BeginArray()` returns `-1`, so consumers use
//!   `HasMoreElements()`.
//! cites: 2.15
//! expect: run
//! output: OK -1 [1;2;3;]
//!   OK -1 [1;2;3;]
//!   OK -1 [1;2;3;]
//!   OK -1 []
//!   OK -1 []
//! notes: The elements are read with ReadInt in the order the loop meets them; the
//!   printed -1 is the BeginArray result the mapping states.
module Conf;

import System.Serialization;

public class Program
  public static method Main()
    Console.WriteLine(L("[1, 2, 3]"));
    Console.WriteLine(L("[1,2,3]"));
    Console.WriteLine(L("[ 1 , 2 , 3 ]"));
    Console.WriteLine(L("[]"));
    Console.WriteLine(L("[ ]"));
  end Main;

  private static method L(j: string): string
    var r: string := "";
    try
      using var d := new JsonDeserializer(j);
      var n := d.BeginArray();
      var acc: string := "";
      while d.HasMoreElements() do
        acc := acc + d.ReadInt().ToString() + ";";
      end;
      d.EndArray();
      r := $"OK {n} [{acc}]";
    catch
      SerializationException => r := "SER";
    end;
    return r;
  end L;
end Program;
