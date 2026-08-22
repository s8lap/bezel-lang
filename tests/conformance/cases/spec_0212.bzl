public method TakeOwnership(owned resource: Stream)
  _resource := resource;     // moves into owned field; the previous
                             // _resource is disposed by the assignment (§28.7)
end TakeOwnership;

public static method NewBuffer(size: int): owned Array<byte>
  return new Array<byte>(size, 0);   // sized ctor (stdlib §4.4); owned by virtue of new, returned as owned
end NewBuffer;
