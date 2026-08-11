var port     := int.ParseOrNull(raw) ?? 8080;
var greeting := config.GetOrNull("greeting") ?? "hello";
