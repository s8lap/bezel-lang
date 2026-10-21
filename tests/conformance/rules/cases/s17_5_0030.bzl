//! case: s17_5_0030
//! doc: spec
//! section: 17.5
//! title: an int left operand is rejected against a Dictionary<string, int> key
//! rule: "the left operand is admitted exactly when it is admissible as that call's argument under the ordinary conversion rules" ([§17.5]); the `Contains(key: K)` of `Dictionary<K, V>` takes the key type (stdlib §4.8), and `int` does not implicitly convert to `string` ([§5.1]).
//! cites: 17.5, 5.1
//! expect: reject
//! diagnostic: implicitly convert
module Conf;

public class Program
  public static method Main()
    var d := new Dictionary<string, int>();
    d["a"] := 1;
    var found := 1 in d;
    Console.WriteLine(found);
  end Main;
end Program;
