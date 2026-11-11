//! case: s28_2_4603
//! doc: spec
//! section: 28.2
//! title: a reassignment on one of two killing arms does not lift the error for an alias
//! rule: A reassignment of the variable between the kill and the use does not lift the error
//! cites: 28.3
//! expect: reject
//! diagnostic: reaches this use
module Conf;

public class Customer
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Customer;

public record Pair(Item: Customer?);

public static class Sink
  public static method TakePair(p: Pair): int => 1;
end Sink;

public class Program
  public static method Main()
    var c := new Customer("ann");
    var b := c;
    var k := "x".Length;
    if k > 0 then
      c.Dispose();
      c := new Customer("n");
    else
      c.Dispose();
    end;
    Console.WriteLine(b.Name);
  end Main;
end Program;
