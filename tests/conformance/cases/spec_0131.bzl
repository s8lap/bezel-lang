public static method Render(users: Array<User>): string
  record NameAge(Name: string, Age: int);   // local: visible only to end Render
  var rows := users.Select(lambda(u) => new NameAge(u.Name, u.Age)).ToArray();
  var sb := new StringBuilder();
  for var row in rows do
    sb.AppendLine($"{row.Name}: {row.Age}");
  end;
  return sb.ToString();
end Render;
