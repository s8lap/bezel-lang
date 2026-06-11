public method DescribeEnum(t: Type)
  if not t.IsEnum then return; end;
  var names := t.EnumNames;
  var values := t.EnumValues;
  for var i in 0..<names.Length do
    Console.WriteLine($"{names[i]} = {values[i]}");
  end;
end DescribeEnum;
