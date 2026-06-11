// ERROR: the while loop may run zero times, leaving _resource moved
//        but unreassigned.
public method WrongLoop(): owned Stream
  var old := move _resource;
  while SomeCondition() do
    _resource := MakeStream();
    break;
  end;
  return old;
end WrongLoop;
