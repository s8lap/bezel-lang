//! case: l2_15_0022
//! doc: stdlib
//! section: 2.15
//! title: ReadFloat reads a JSON number of one hundred digits
//! rule: `ReadFloat` reads a JSON number of any length, its value rounded once to `double`
//! cites: 2.15, 9.2
//! expect: run
//! issue: 7340
//! stdout: near

module Conf;

import System.Serialization;

record R(V: double): ISerializable, IDeserializable;

public class Program
  public static method Main()
    var digits := "1";
    for var k in 0..<99 do
      digits := digits + "0";
    end for;
    var expected := 1.0;
    for var k in 0..<99 do
      expected := expected * 10.0;
    end for;
    try
      using var d := new JsonDeserializer("{\"V\": " + digits + "}");
      var v := R.Deserialize(d);
      var ratio := v.V / expected;
      if ratio > 0.999 and ratio < 1.001 then
        Console.WriteLine("near");
      else
        Console.WriteLine("far");
      end;
    catch
      SerializationException => Console.WriteLine("SER");
    end;
  end Main;
end Program;
