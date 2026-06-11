public method RightOrder(): owned Stream
  var fresh := DangerousFactory();   // may throw — but no move yet
  var old := move _resource;          // cannot throw
  _resource := fresh;                  // cannot throw (assignment, not call)
  return old;                          // cannot throw
end RightOrder;
