//! case: s28_2_4601
//! doc: spec
//! section: 28.2
//! title: a kill inside a do block that always runs makes the later use of an alias an error
//! rule: has been so disposed on every path to the use
//! cites: 28.3
//! expect: reject
//! diagnostic: disposed variable
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
    do
      c.Dispose();
    end;
    Console.WriteLine(b.Name);
  end Main;
end Program;
