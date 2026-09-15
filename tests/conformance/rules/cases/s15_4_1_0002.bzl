//! case: s15_4_1_0002
//! doc: spec
//! section: 15.4.1
//! title: a matching FromCollectionLiteral without the conformance is rejected at the declaration
//! rule: A class that declares a `public static` method named
//!   `FromCollectionLiteral` whose signature matches either interface's member
//!   at `TSelf` = the class, but that conforms to neither interface, is a
//!   compile error at that declaration naming the missing conformance
//!   (§15.4.1).
//! cites: 15.4.1
//! expect: reject
//! diagnostic: conformance
module Conf;

public class Bag<T>
  private owned _items: Array<T>;

  public constructor(capacity: int)
    _items := new Array<T>(capacity);
  end constructor;

  public property Count: int => _items.Count;

  public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned Bag<T>
    return new Bag<T>(elements.Length);
  end FromCollectionLiteral;
end Bag;
