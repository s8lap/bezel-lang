//! case: s28_2_4604
//! doc: spec
//! section: 28.2
//! title: passing a borrow-bearing record after the kill of its source is an error
//! rule: a read or member access through it, passing it, re-borrowing it
//! cites: 28.3, 9.7
//! expect: reject
//! diagnostic: use through a local borrow
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
    var p := new Pair(c);
    c.Dispose();
    Console.WriteLine(Sink.TakePair(p));
  end Main;
end Program;
