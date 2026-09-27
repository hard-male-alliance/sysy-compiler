# Implemented SysY language and grammar

This is the compiler's language-facing contract as of the current source tree. The repository's [SysY 2022 definition](task/SysY2022语言定义-V1.md) and [grammar supplement](task/SysY文法补充说明.md) are the primary task sources. The supplement corrects array-parameter dimensions, admits `!` and logical/relational operations in any expression, interprets `++a` as two unary plus operators, and exempts `putf` from the required runtime. The table and grammar below describe the current lexer (`src/lexer.cpp`), parser (`src/parser.cpp`), and semantic pass (`src/semantic.cpp`); they should be updated together when those implementations change.

## Lexical structure

The following regular expressions are descriptive ASCII patterns, not a claim that the scanner is generated from regexes. `digit = [0-9]`, `nz = [1-9]`, `oct = [0-7]`, `hex = [0-9a-fA-F]`, `sign = [+-]?`.

| Token | Accepted spelling / descriptive regex | Notes |
| --- | --- | --- |
| Identifier | `[A-Za-z_][A-Za-z_0-9]*` | ASCII only. Exact words `int float void const if else while break continue return` are keywords. |
| Decimal integer | `[1-9][0-9]*` | No suffix. The full magnitude is preserved until semantic conversion, including the `-2147483648` case. |
| Octal integer | `0[0-7]*` | `08` and `09` are lexical errors if not part of a float. |
| Hex integer | `0[xX][0-9a-fA-F]+` | No suffix. |
| Decimal float | `(([0-9]+\.[0-9]*|\.[0-9]+)([eE][+-]?[0-9]+)?|[0-9]+[eE][+-]?[0-9]+)` | Examples: `1.`, `.5`, `1.2e-3`, `1e3`. A literal is a SysY `float` (binary32), not C `double`. |
| Hex float | `0[xX]([0-9a-fA-F]+(\.[0-9a-fA-F]*)?|\.[0-9a-fA-F]+)[pP][+-]?[0-9]+` | The binary `p` exponent is mandatory. |
| String | `"([^"\\\n]|\\[^\n])*"` | Lexed, but currently rejected by semantic analysis; only the optional `putf` extension would use it. |
| Whitespace | spaces, tabs, CR, LF, form feed, vertical tab | Ignored. |
| Comments | `//` to LF; `/*` to first `*/` | Ignored; block comments are not nested. Unterminated block comments are errors. |

The scanner recognizes `+ - * / % ! < > <= >= == != && || = , ; ( ) [ ] { }`. Single `&` and `|`, numeric suffixes, malformed exponents, and unexpected characters are lexical errors. A leading `-` is a unary operator, not part of a numeric token; `++a` is parsed as `+(+a)`, not C's increment. Decimal and hexadecimal float literal parsing rejects non-finite results in the semantic pass.

## Context-free syntax

This EBNF uses `{...}` for repetition and `[...]` for optional syntax. `Expr` deliberately includes logical and comparison operators at **every** expression site, following the supplement rather than the base document's narrower `Exp -> AddExp`. Binary operators are left-associative at their indicated precedence; unary operators are right-associative. Parentheses override precedence.

```ebnf
Program       = { Declaration | Function } ;
Declaration   = [ "const" ] BaseType Definition { "," Definition } ";" ;
BaseType      = "int" | "float" ;
Definition    = Identifier { "[" Expr "]" } [ "=" Initializer ] ;
Initializer   = Expr | "{" [ Initializer { "," Initializer } ] "}" ;
Function      = ( BaseType | "void" ) Identifier "(" ( "void" | [ Parameter { "," Parameter } ] ) ")" ( ";" | Block ) ;
Parameter     = BaseType [ Identifier ] [ "[" "]" { "[" Expr "]" } ] ;
Block         = "{" { Declaration | Statement } "}" ;
Statement     = Variable "=" Expr ";" | [ Expr ] ";" | Block
              | "if" "(" Expr ")" Statement [ "else" Statement ]
              | "while" "(" Expr ")" Statement
              | "break" ";" | "continue" ";" | "return" [ Expr ] ";" ;
Expr          = LogicOr ;
LogicOr       = LogicAnd { "||" LogicAnd } ;
LogicAnd      = Equality { "&&" Equality } ;
Equality      = Relation { ( "==" | "!=" ) Relation } ;
Relation      = Sum { ( "<" | ">" | "<=" | ">=" ) Sum } ;
Sum           = Product { ( "+" | "-" ) Product } ;
Product       = Unary { ( "*" | "/" | "%" ) Unary } ;
Unary         = ( "+" | "-" | "!" ) Unary | Primary ;
Primary       = "(" Expr ")" | Number | String | Variable | Call ;
Variable      = Identifier { "[" Expr "]" } ;
Call          = Identifier "(" [ Expr { "," Expr } ] ")" ;
Number        = Integer | Float ;
```

`Variable = Expr` is *not* a general assignment expression: assignment is a statement and its left side must parse as a variable/array element. The parser accepts an `Expr` syntactically in an array bound or initializer; semantic analysis requires constant `int` array dimensions and compile-time values for constants and global initializers. The first bound of an array parameter is omitted (`[]`), while all trailing bounds must be constant. Parameter names may be omitted only in a prototype, not a definition. `f(void)` and `f()` are equivalent no-argument spellings. A semicolon terminates a prototype; a block supplies the definition. `else` binds to the nearest unmatched `if`. The grammar lists `String` to describe parsing, but no source program using a string expression passes current semantics.

## Semantic boundaries

| Feature | Current behavior |
| --- | --- |
| Entry point and source order | Exactly one `int main()` definition with no parameters. Compatible prototypes may repeat, and one matching definition is required by the end of the single translation unit, except for runtime builtins. A prototype makes a later definition callable before its body appears; without a prototype, forward calls remain invalid. Conflicting signatures, duplicate definitions, and unresolved user prototypes are errors. Self-recursion works. |
| Types and arrays | Signed 32-bit `int`, binary32 `float`, `void` function result; row-major multidimensional arrays. Scalar `int`/`float` conversions are implicit. Array arguments match element type, rank, and trailing extents. Zero-length dimensions are accepted: the array has **zero logical elements**, although code generation reserves one physical 32-bit backing word so it has a representable address. Access to any element of a zero-extent array is out of bounds and has no defined result. Arrays exceeding the implementation's shape limit are rejected. |
| Constants and initialization | `const` needs an initializer. Compile-time constants may be used as dimensions. Global objects without explicit initializers are zero-filled; uninitialized locals are not promised zero. Braced array initializers align nested groups to subarray boundaries and zero-fill omitted elements. If an array has zero logical elements, only `{}` is accepted as an explicit initializer; it creates no element values. |
| Operations/control | C-like arithmetic/comparisons; `%` requires integer operands. `&&` and `||` short-circuit. `break`/`continue` require an enclosing `while`. A non-void function that falls through has an undefined result per the task document, not a compiler-invented value. |
| Runtime calls | Builtins include `getint`, `getch`, `getfloat`, `getarray`, `getfarray`, `putint`, `putch`, `putfloat`, `putarray`, `putfarray`, `starttime`, and `stoptime`. Timer calls lower to `_sysy_starttime(line)` / `_sysy_stoptime(line)`. `putf` is not implemented. |

This is not C: there are no `for` loops, `switch`, structs, pointers in source syntax, casts, object `extern` declarations, or increment/decrement operators. Function prototypes are the sole standalone declaration extension beyond the base SysY grammar. The task document mentions a “tensor type” in its coursework checklist but supplies no independent syntax or semantics beyond multidimensional arrays; this compiler implements the specified arrays, not an invented tensor dialect. For deeper rationale and unresolved specification ambiguities, see the [task contract analysis](architecture/sysy2022-contract.md) and [semantic model](architecture/semantic-model.md).
