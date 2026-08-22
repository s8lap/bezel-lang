public method RightOrder(): owned Stream
  var fresh := DangerousFactory();   // may throw — but no move yet
  var old := move _resource;          // cannot throw
  _resource := fresh;                  // cannot throw (the slot is moved-from:
                                       //   no drop-on-overwrite runs)
  return old;                          // cannot throw
end RightOrder;
