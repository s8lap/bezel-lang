# n19-template

*Area:* text

A tiny template renderer: replace `{{name}}` placeholders from a `Dictionary<string,string>`; `{{name|upper}}` applies an upper-case filter; an unknown key renders as `{{?key}}`; `{{{{` is an escaped `{{`. Render `Hello {{name|upper}}, you have {{count}} new {{things}}. {{{{literal}}` with name=ada, count=3 -> `Hello ADA, you have 3 new {{?things}}. {{literal}}`.
