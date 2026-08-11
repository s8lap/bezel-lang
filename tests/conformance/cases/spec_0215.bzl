// ERROR: if DangerousFactory throws, _resource is moved but never reassigned;
//        the exception edge is an unrestored exit.
public method WrongOrder(): owned Stream
  var old := move _resource;
  var fresh := DangerousFactory();
  _resource := fresh;
  return old;
end WrongOrder;
