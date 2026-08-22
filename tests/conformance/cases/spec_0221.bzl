public method RightLoop(): owned Stream
  var fresh := MakeStream();
  while SomeCondition() do
    fresh := MakeStream();   // refine fresh in the loop
  end;
  var old := move _resource;
  _resource := fresh;
  return old;
end RightLoop;
