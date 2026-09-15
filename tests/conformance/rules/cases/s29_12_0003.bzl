//! case: s29_12_0003
//! doc: spec
//! section: 29.12
//! title: the two-loop remove-while-iterating idiom avoids structural modification during iteration
//! rule: The standard pattern for removing during iteration: collect
//!   matches in a first pass over the source, then remove them in a second
//!   pass over the matches.
//! cites: 29.12
//! expect: run
//! stdout: 1
module Conf;

public class Transaction
  public property IsExpired: bool;

  public constructor(expired: bool)
    IsExpired := expired;
  end constructor;
end Transaction;

public class Program
  public static method Main()
    var transactions: Array<Transaction> := [
      new Transaction(true),
      new Transaction(false),
      new Transaction(true)
    ];
    var toRemove := new Array<Transaction>();
    for var t in transactions do
      if t.IsExpired then toRemove.Add(t); end;
    end;
    for var t in toRemove do
      _ := transactions.Remove(t);
    end;
    Console.WriteLine(transactions.Count);
  end Main;
end Program;
