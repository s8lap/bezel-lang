case shape of
  Circle(_) => "circle",
  Rectangle(_, _) => "rectangle",
  _ => "other"
end;
