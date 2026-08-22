method CloneBuffer(): Buffer                     // WRONG
  var fresh := new Buffer();
  return fresh;                                  // ERROR: a fresh owned value through a non-`owned` return
end CloneBuffer;

public static method Create(host: string): Connection    // ERROR at the DECLARATION
