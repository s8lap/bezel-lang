//! case: s30_5_0024
//! doc: spec
//! section: 30.5
//! title: an async Task<T> body returning a plain class-typed parameter is rejected
//! rule: **No fourth kind: a non-`await` borrow return is rejected.** A
//!   `return` in an `async` body — an `async` method's or an `async` lambda's
//!   — whose return type is `Task<T>` for class or interface T, whose operand
//!   is a borrow and not a `return await`: a bare read of a field of `this` or
//!   of any object, a plain parameter, … — is a compile error at the return
//!   site
//! cites: 30.5, 28.3
//! expect: reject
//! diagnostic: borrow
//! notes: accepting twin s30_5_0025 — the one edit returns a fresh owned value
//!   (`new Item(i.Name)`, kind 1) instead of the parameter.
module Conf;

public class Item
  public Name: string;
  public constructor(name: string) Name := name; end constructor;
end Item;

public class Program
  public async method EchoAsync(i: Item): Task<Item>
    await Task.Delay(0);
    return i;
  end EchoAsync;
end Program;
