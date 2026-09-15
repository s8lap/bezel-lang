//! case: s23_1_0001
//! doc: spec
//! section: 23.1
//! title: the four auto-property forms are admitted
//! rule: An auto-property is read-write, read-write with a restricted setter
//!   (`private set`), read-only (`readonly`), or read-only with an initializer;
//!   the compiler generates the backing field.
//! cites: 23.1
//! expect: accept
module Conf;

public class Account
  public property Name: string;
  public property Id: int private set;
  public readonly property AccountNumber: string;
  public readonly property Pi: double := 3.141592653589793;

  public constructor(name: string, id: int, number: string)
    Name := name;
    Id := id;
    AccountNumber := number;
  end constructor;
end Account;
