//! case: s28_11_9608
//! doc: spec
//! section: 28.11
//! title: a borrow declared in a nested block stored into a collection of the wider scope is rejected
//! rule: or is a local of strictly wider scope — is rejected at the store site
//! cites: 28.11, 28.3
//! expect: reject
//! diagnostic: outlive
//! issue: 9182
module Conf;

public class C
  public method Put(k: string)
    var outer := new Dictionary<string, Dictionary<string, int>>();
    if k = "a" then
      var inner := new Dictionary<string, int>();
      inner["x"] := 1;
      outer[k] := inner;
    end if;
    Console.WriteLine(outer.Count);
  end Put;
end C;
