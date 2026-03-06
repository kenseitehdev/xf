# xf

**xf** is a systems-oriented data and stream processing scripting language. It combines the record-splitting pipeline model of awk with typed variables, first-class collections, a state/type value system, shell-pipe integration, and a module library — all accessible from an interactive REPL or as a command-line tool for file and stream processing.

---

## Table of Contents

1. [Invocation](#invocation)
2. [Value System](#value-system)
3. [Types](#types)
4. [Variables & Declarations](#variables--declarations)
5. [Operators](#operators)
6. [Control Flow](#control-flow)
7. [Functions](#functions)
8. [Collections](#collections)
9. [Pattern-Action Rules](#pattern-action-rules)
10. [Record Variables](#record-variables)
11. [Stream & I/O](#stream--io)
12. [Output Formatting](#output-formatting)
13. [Substitution & Transliteration](#substitution--transliteration)
14. [Concurrency](#concurrency)
15. [Modules](#modules)
16. [Built-in Functions](#built-in-functions)
17. [REPL](#repl)
18. [Examples](#examples)
19. [Classic Programs](#classic-programs)

---

## Invocation

```
xf                          # start interactive REPL
xf -e 'expr'                # execute inline expression
xf -f script.xf             # execute script file
xf -e 'expr' input.txt      # inline expression with file input
xf -f script.xf input.txt   # script with file input
```

**Flags**

| Flag      | Description                                  |
|-----------|----------------------------------------------|
| `-e expr` | inline expression (may be repeated)          |
| `-f file` | script file (`.xf`)                          |
| `-n`      | suppress implicit print                      |
| `-p`      | print every input record                     |
| `-j N`    | parallel schedulables (default: 1)           |
| `-s`      | strict mode — NAV is treated as ERR          |
| `-l`      | lenient mode — NAV does not propagate        |
| `-h`      | show help                                    |
| `-v`      | show version                                 |

---

## Value System

Every value in xf carries two orthogonal axes: **state** and **type**. State is primary — it takes precedence over type in every operation. A value with a non-`OK` state propagates that state through expressions rather than producing a result.

### The 7 States

States form a one-way lifecycle. Once a value reaches a **terminal state** it cannot change. Transitions are atomic — in concurrent code, only one thread wins the collapse race.

```
UNDETERMINED → UNDEF → { OK, ERR, NAV, NULL, VOID }
                              ↑ terminal — no further change possible
```

| State           | Terminal | Meaning                                                               |
|-----------------|----------|-----------------------------------------------------------------------|
| `UNDETERMINED`  | no       | Not yet processed. Pre-collapse — the scheduler has not touched it.   |
| `UNDEF`         | no       | Declared but not yet assigned. Reading such a variable produces NAV.  |
| `OK`            | yes      | Value is valid and fully usable.                                      |
| `ERR`           | yes      | Value carries a fault. Propagates through all downstream expressions. |
| `NAV`           | yes      | Return was expected but nothing came back. Propagates like ERR.       |
| `NULL`          | yes      | No return was expected and none was given. Silent empty.              |
| `VOID`          | yes      | No return was expected but a value leaked out anyway.                 |

**Error states** (`ERR`, `NAV`) propagate automatically — any arithmetic, comparison, or function call on an error-state value short-circuits and returns the same error state without executing. This mirrors NaN propagation in IEEE floats but applies to the entire type system.

**Pending states** (`UNDETERMINED`, `UNDEF`) block further collapse. In the scheduler, `UNDETERMINED` means queued for execution and `UNDEF` means currently running.

### State Transitions in Practice

```xf
num x               # UNDEF  — declared, not yet assigned
x = 42              # OK     — valid value
x = num("bad")      # NAV    — coercion failed, state collapses to NAV
x + 10              # NAV    — propagated, no addition performed
```

A `void` function that runs to completion sets its return to `NULL`. A typed function that exits without a `return` gives the caller `NAV`.

```xf
void fn log(str msg) {
    print msg         # NULL — no return value expected or given
}

num fn broken(str s) {
    print s           # no return → caller receives NAV
}
```

### Inspecting State

```xf
x.state          # returns the state: OK, NAV, ERR, NULL, VOID, UNDEF, UNDETERMINED
x.type           # returns the type: num, str, arr, map, set, fn, void
x.len            # element count for str, arr, map, or set

if x.state == OK  { print "valid" }
if x.state == NAV { print "missing" }
if x.state == ERR { print "faulted" }

# guard before use
num result = parse(input)
if result.state != OK { exit }
print result * 2
```

### Null Coalescing

```xf
num y = x ?? 0          # use 0 if x is NAV or NULL
str s = name ?? "anon"  # fallback string
```

---

## Types

| Type   | Description                                      |
|--------|--------------------------------------------------|
| `num`  | Double-precision float                           |
| `str`  | Reference-counted UTF-8 string                   |
| `arr`  | Ordered array (heterogeneous elements)           |
| `map`  | Associative array, insertion-ordered             |
| `set`  | Unique-value collection                          |
| `fn`   | First-class callable                             |
| `void` | No-value return type for functions               |

### Type Casts

```xf
num n = num("3.14")      # string → num
str s = str(99)          # num → str
arr a = arr(some_map)    # convert to array
map m = map(some_arr)    # convert to map
```

---

## Variables & Declarations

Variables require an explicit type keyword on first declaration.

```xf
num  count  = 0
str  name   = "alice"
arr  items  = [1, 2, 3]
map  scores = {"alice": 95, "bob": 88}
set  seen   = {"a", "b", "c"}
```

**Uninitialized declaration** — state is `UNDEF` until assigned:

```xf
num result
```

**Walrus operator** `:=` — declare and assign in expression position:

```xf
if (n := compute()) > 0 {
    print n
}
```

---

## Operators

### Arithmetic

```xf
x + y    x - y    x * y    x / y    x % y    x ^ y   # ^ = exponentiation
x += 1   x -= 1   x *= 2   x /= 2   x %= 3
x++      x--      ++x      --x
```

### Comparison

```xf
x == y    x != y    x < y    x > y    x <= y    x >= y
x <=> y                    # three-way: returns -1, 0, or 1
```

### Logical

```xf
x && y    x || y    !x
```

### String Concatenation

```xf
str full = first .. " " .. last
full ..= "!"
```

### Regex Match

```xf
str line = "hello world"
line ~ /world/         # true
line !~ /missing/      # true
line ~ /HELLO/i        # case-insensitive flag
line ~ /^start/m       # multiline flag
```

### Ternary

```xf
str label = score >= 90 ? "pass" : "fail"
```

### Shell Pipe

Pipe a value through a shell command; returns stdout as a string:

```xf
str result = "hello world" | "tr a-z A-Z"    # "HELLO WORLD"
```

### Pipe-to-Function

```xf
result = value |> transform_fn
```

### Matrix / Array Element-wise

```xf
[1,2,3] .+ [4,5,6]    # [5, 7, 9]    element-wise add
[2,4,6] .- [1,2,3]    # [1, 2, 3]    element-wise subtract
[1,2,3] ./ [1,2,1]    # [1, 1, 3]    element-wise divide
A .* B                 # true matrix multiply (nested arrays as rows)
```

---

## Control Flow

### if / elif / else

```xf
if x > 10 {
    print "big"
} elif x > 5 {
    print "medium"
} else {
    print "small"
}
```

### while

```xf
while i < 10 {
    i++
}
```

**Shorthand while** — `condition <> body`:

```xf
i < 10 <> print i++
```

### for-in

```xf
for (item in items) {
    print item
}

for (key in mymap) {
    print key, mymap[key]
}
```

**Shorthand for** — `collection[iter] > body`:

```xf
items[x] > print x
mymap[k] > print k, mymap[k]
```

### next / exit

```xf
next    # skip to next input record (in pattern-action context)
exit    # terminate program
```

### delete

```xf
delete arr[2]         # remove element at index 2
delete map["key"]     # remove key from map
```

---

## Functions

### Declaration

```xf
num fn add(num a, num b) {
    return a + b
}

void fn greet(str name) {
    print "hello, " .. name
}

str fn fmt_score(str player, num score) {
    return sprintf "%s: %g", player, score
}
```

### First-class / Anonymous

```xf
fn square = fn(num x) { return x * x }
num r = square(5)    # 25
```

### Passing Functions as Arguments

```xf
num fn apply(fn f, num x) { return f(x) }

fn double = fn(num x) { return x * 2 }
print apply(double, 21)    # 42
```

---

## Collections

### Arrays

```xf
arr items = [10, 20, 30]
items[0]                  # 10
items.len                 # 3
items[items.len - 1]      # last element

push(items, 40)           # append; also: items.push(40)
pop(items)                # remove + return last
shift(items)              # remove + return first
unshift(items, 5)         # prepend; also: items.unshift(5)
remove(items, 1)          # remove by index
```

### Maps

```xf
map scores = {"alice": 95, "bob": 88}
scores["alice"]                  # 95
scores["carol"] = 72             # insert/update

has(scores, "alice")             # 1  (also: scores.has("alice"))
keys(scores)                     # ["alice", "bob", "carol"]
values(scores)                   # [95, 88, 72]

delete scores["bob"]
scores.len                       # 2
```

### Sets

```xf
set seen = {"a", "b", "c"}
seen.has("a")                    # 1
has(seen, "z")                   # 0
```

---

## Pattern-Action Rules

xf scripts can define `BEGIN`, `END`, and pattern-action rules that run against each line of input. The program structure mirrors awk.

```xf
BEGIN {
    FS = ","
    print "starting"
}

# runs for every record where field 1 is greater than 100
$1 > 100 {
    print $2, $3
}

# regex pattern — runs for lines that match
/error/ {
    print "FOUND:", $0
}

# bare block — runs for every record
{
    count++
}

END {
    print "total records:", count
}
```

Running with input:

```
xf -f script.xf data.csv
```

---

## Record Variables

These implicit variables are updated automatically for each input record:

| Variable  | Description                                      |
|-----------|--------------------------------------------------|
| `$0`      | Full current record (the whole line)             |
| `$1`–`$N` | Individual fields, split by `FS`                 |
| `NR`      | Global record number (across all files)          |
| `FNR`     | Per-file record number                           |
| `NF`      | Number of fields in the current record           |
| `FS`      | Field separator (default: whitespace)            |
| `RS`      | Record separator                                 |
| `OFS`     | Output field separator                           |
| `ORS`     | Output record separator                          |

```xf
BEGIN { FS = "\t" }

$3 ~ /active/ {
    printf "user %s joined on %s\n", $1, $2
}
```

---

## Stream & I/O

### Shell Pipe Operator

Write a value to a shell command's stdin; get stdout back as a string:

```xf
str out   = "hello world" | "tr a-z A-Z"     # "HELLO WORLD"
str lines = data | "sort -u | head -10"
```

### File I/O Functions

```xf
read("file.txt")              # read entire file → str
lines("file.txt")             # read file → arr of lines (stripped)
write("out.txt", content)     # write str to file → 1 or 0
append("log.txt", entry)      # append str to file → 1 or 0
```

### print / printf

```xf
print x                       # prints value + newline
print x, y, z                 # space-separated
printf "%s scored %g\n", name, score

# output redirection
print x > "output.txt"        # overwrite file
print x >> "output.txt"       # append to file
print x | "gzip > out.gz"     # pipe to command
```

---

## Output Formatting

`outfmt` sets the structured output mode for subsequent `print` statements:

```xf
outfmt "text"     # plain text (default)
outfmt "csv"      # comma-separated values
outfmt "tsv"      # tab-separated values
outfmt "json"     # JSON
```

---

## Substitution & Transliteration

Applied to the current input record (`$0`) in pattern-action context:

```xf
s/pattern/replacement/        # replace first match
s/pattern/replacement/g       # replace all matches (global)
s/pattern/replacement/i       # case-insensitive
y/abc/ABC/                    # transliterate: map chars from→to
tr/abc/ABC/                   # same as y///
```

---

## Concurrency

`spawn` schedules a function call to run concurrently. `join` blocks until all spawned jobs complete. Control the number of parallel workers with `-j N`.

```xf
void fn process(str file) {
    arr data = lines(file)
    num total = 0
    data[row] > total += num(row)
    printf "%s: %g\n", file, total
}

spawn process("a.txt")
spawn process("b.txt")
spawn process("c.txt")
join
```

```
xf -j 4 -f script.xf
```

---

## Modules

### import

```xf
import "utils.xf"
```

The `core` namespace is registered automatically at startup. Access it directly:

```xf
num r = core.math.sqrt(16)
str s = core.str.upper("hello")
```

---

### core.math

| Function / Constant         | Description                                   |
|-----------------------------|-----------------------------------------------|
| `core.math.sin(x)`          | Sine                                          |
| `core.math.cos(x)`          | Cosine                                        |
| `core.math.tan(x)`          | Tangent                                       |
| `core.math.asin(x)`         | Arc sine                                      |
| `core.math.acos(x)`         | Arc cosine                                    |
| `core.math.atan(x)`         | Arc tangent                                   |
| `core.math.atan2(y, x)`     | Two-argument arc tangent                      |
| `core.math.sqrt(x)`         | Square root                                   |
| `core.math.pow(x, y)`       | x to the power y                              |
| `core.math.exp(x)`          | e^x                                           |
| `core.math.log(x)`          | Natural log                                   |
| `core.math.log2(x)`         | Log base 2                                    |
| `core.math.log10(x)`        | Log base 10                                   |
| `core.math.abs(x)`          | Absolute value                                |
| `core.math.floor(x)`        | Floor                                         |
| `core.math.ceil(x)`         | Ceiling                                       |
| `core.math.round(x)`        | Round to nearest                              |
| `core.math.int(x)`          | Truncate to integer                           |
| `core.math.min(a, b)`       | Minimum                                       |
| `core.math.max(a, b)`       | Maximum                                       |
| `core.math.clamp(v, lo, hi)`| Clamp v between lo and hi                     |
| `core.math.rand()`          | Random float in [0, 1)                        |
| `core.math.srand(seed)`     | Seed the RNG                                  |
| `core.math.PI`              | 3.14159265...                                 |
| `core.math.E`               | 2.71828182...                                 |
| `core.math.INF`             | Positive infinity                             |
| `core.math.NAN`             | Not-a-number                                  |

---

### core.str

| Function                              | Description                                   |
|---------------------------------------|-----------------------------------------------|
| `core.str.len(s)`                     | Character length                              |
| `core.str.upper(s)`                   | Uppercase                                     |
| `core.str.lower(s)`                   | Lowercase                                     |
| `core.str.trim(s)`                    | Strip leading and trailing whitespace         |
| `core.str.ltrim(s)`                   | Strip leading whitespace                      |
| `core.str.rtrim(s)`                   | Strip trailing whitespace                     |
| `core.str.substr(s, start)`           | Substring from start                          |
| `core.str.substr(s, start, len)`      | Substring with length                         |
| `core.str.index(s, needle)`           | Byte offset of needle, or -1                  |
| `core.str.contains(s, needle)`        | 1 if needle found, else 0                     |
| `core.str.starts_with(s, prefix)`     | 1 if s starts with prefix                     |
| `core.str.ends_with(s, suffix)`       | 1 if s ends with suffix                       |
| `core.str.replace(s, old, new)`       | Replace first occurrence                      |
| `core.str.replace_all(s, old, new)`   | Replace all occurrences                       |
| `core.str.repeat(s, n)`               | Repeat string n times                         |
| `core.str.reverse(s)`                 | Reverse string                                |
| `core.str.sprintf(fmt, ...)`          | Printf-style format                           |
| `core.str.concat(a, b, ...)`          | Join any number of strings                    |
| `core.str.comp(a, b)`                 | strcmp-style: -1, 0, or 1                     |

---

### core.os

| Function                        | Description                                     |
|---------------------------------|-------------------------------------------------|
| `core.os.exec(cmd)`             | Run shell command, return exit code             |
| `core.os.exit(code)`            | Exit program with code                          |
| `core.os.time()`                | Unix timestamp in seconds                       |
| `core.os.env(name)`             | Get environment variable → str                  |
| `core.os.read(path)`            | Read entire file → str                          |
| `core.os.write(path, data)`     | Write str to file → 1 or 0                      |
| `core.os.append(path, data)`    | Append str to file → 1 or 0                     |
| `core.os.lines(path)`           | Read file → arr of lines (newlines stripped)    |
| `core.os.run(cmd)`              | Run shell command → stdout as str               |
| `core.os.run_lines(cmd)`        | Run shell command → arr of output lines         |

---

## Built-in Functions

These are available globally without any module import.

### Math

```xf
sin(x)    cos(x)    sqrt(x)    abs(x)    int(x)
rand()              # float in [0, 1)
srand(seed)         # seed the RNG
```

### String

```xf
len(s)                     # character length
toupper(s)
tolower(s)
trim(s)
substr(s, start)
substr(s, start, len)
index(s, needle)           # byte offset or -1
sprintf(fmt, ...)          # printf-style formatting
column(s, n)               # extract nth whitespace-delimited column
split(s, sep)              # split string → arr
sub(pat, rep, target)      # replace first regex match in target
gsub(pat, rep, target)     # replace all regex matches in target
match(s, /re/)             # test match; sets $match and $captures
```

### Collections

```xf
len(coll)                  # element count for arr, map, set, or str
push(arr, val)             # append to arr, return arr
pop(arr)                   # remove + return last element
shift(arr)                 # remove + return first element
unshift(arr, val)          # prepend to arr, return arr
remove(coll, idx)          # remove element by index or map key
has(coll, key)             # membership test → 1 or 0
keys(map)                  # insertion-ordered key arr
values(map)                # insertion-ordered value arr
```

### I/O & System

```xf
read(path)                 # read file → str
lines(path)                # read file → arr of lines
write(path, data)          # write file → 1 or 0
append(path, data)         # append to file → 1 or 0
system(cmd)                # run shell command, return exit code
```

---

## REPL

Start the REPL with no arguments:

```
xf
```

**Line editing**

| Key              | Action                       |
|------------------|------------------------------|
| `←` / `→`       | Move cursor                  |
| `↑` / `↓`       | Navigate history             |
| `Home` / `End`   | Jump to line start/end       |
| `Ctrl-A` / `Ctrl-E` | Same as Home/End          |
| `Ctrl-K`         | Kill to end of line          |
| `Ctrl-U`         | Kill to start of line        |
| `Ctrl-W`         | Kill previous word           |
| `Ctrl-C`         | Cancel current line          |
| `Ctrl-D`         | Exit on empty line           |

**Commands**

| Command       | Description                              |
|---------------|------------------------------------------|
| `:help`       | Show commands                            |
| `:state`      | Show all bindings and their values       |
| `:type x`     | Show type and state of variable `x`      |
| `:load f`     | Load `.xf` file into current session     |
| `:reload`     | Reload the last loaded file              |
| `:history`    | Show input history                       |
| `:disasm`     | Show VM bytecode disassembly             |
| `:clear`      | Reset environment                        |
| `:quit`       | Exit                                     |

History is persisted to `~/.xf_history` across sessions.

---

## Examples

### Hello World

```xf
print "hello, world"
```

### Sum a Column in a CSV File

```xf
# xf -f sum.xf data.csv
BEGIN { FS = "," }
{ total += num($2) }
END   { print "total:", total }
```

### Word Frequency Counter

```xf
{
    for (word in split($0, " ")) {
        freq[word]++
    }
}
END {
    keys(freq)[k] > printf "%d\t%s\n", freq[k], k
}
```

### Filter Lines by Regex

```xf
# xf -e '/error/ { print NR, $0 }' app.log
/error/ { print NR, $0 }
```

### State-Aware Error Handling

```xf
num val = num("not-a-number")

if val.state == NAV {
    print "conversion failed, using default"
    val = 0
}

# coalesce: fallback if NAV or NULL
num safe = val ?? -1
```

### Shell Pipeline Integration

```xf
str out    = core.os.run("cut -d: -f1 /etc/passwd")
str sorted = out | "sort"
arr users  = split(sorted, "\n")
users[u] > print u
```

### Read, Transform, Write a File

```xf
arr data = core.os.lines("/tmp/input.txt")
str result = ""
data[line] > result ..= core.str.upper(line) .. "\n"
core.os.write("/tmp/output.txt", result)
```

### Map and Set Operations

```xf
map inv = {"apples": 5, "bananas": 3}
inv["oranges"] = 8
delete inv["bananas"]

print has(inv, "apples")    # 1
print keys(inv)             # [apples, oranges]
print values(inv)           # [5, 8]

set visited = {"home", "about"}
print visited.has("home")   # 1
```

### Matrix Arithmetic

```xf
arr A = [[1,2],[3,4]]
arr B = [[5,6],[7,8]]

arr C = A .* B              # matrix multiply
arr D = A .+ B              # element-wise add → [[6,8],[10,12]]
arr E = A ./ [[1,2],[1,2]]  # element-wise divide
```

### Anonymous Functions

```xf
fn double = fn(num x) { return x * 2 }
fn apply  = fn(fn f, num x) { return f(x) }

print apply(double, 21)     # 42
```

### Timed Benchmark

```xf
num t0 = core.os.time()
num i = 0
i < 1000000 <> i++
printf "elapsed: %gs\n", core.os.time() - t0
```

### outfmt Structured Output

```xf
# xf -f report.xf data.tsv
BEGIN { FS = "\t"; outfmt "json" }
NR > 1 {
    print $1, $2, $3
}
```

### Spawn Parallel Jobs

```xf
void fn crunch(str file) {
    arr rows = lines(file)
    num total = 0
    rows[r] > total += num(r)
    printf "%s: %g\n", file, total
}

spawn crunch("a.txt")
spawn crunch("b.txt")
spawn crunch("c.txt")
join
```

```
xf -j 4 -f script.xf
```

### Math Module

```xf
num angle = core.math.PI / 4
num s = core.math.sin(angle)
num c = core.math.cos(angle)
printf "sin(π/4) = %.4f, cos(π/4) = %.4f\n", s, c

num clamped = core.math.clamp(score, 0, 100)
```

### String Module

```xf
str raw = "  Hello, World!  "
str clean = core.str.trim(raw)
str lower = core.str.lower(clean)
num found = core.str.starts_with(lower, "hello")   # 1

str joined = core.str.concat("a", "-", "b", "-", "c")  # "a-b-c"
num order  = core.str.comp("alpha", "beta")             # -1
```

---

## Classic Programs

### FizzBuzz

```xf
num i = 1
while i <= 100 {
    if i % 15 == 0      { print "FizzBuzz" }
    elif i % 3 == 0     { print "Fizz" }
    elif i % 5 == 0     { print "Buzz" }
    else                { print i }
    i++
}
```

Shorthand form using `<>`:

```xf
num i = 1
i <= 100 <> {
    str out = (i % 3 == 0 ? "Fizz" : "") .. (i % 5 == 0 ? "Buzz" : "")
    print (out.len > 0 ? out : str(i))
    i++
}
```

---

### Bubble Sort

```xf
arr fn bubble_sort(arr a) {
    num n = a.len
    num i = 0
    while i < n - 1 {
        num j = 0
        while j < n - i - 1 {
            if a[j] > a[j+1] {
                num tmp = a[j]
                a[j]   = a[j+1]
                a[j+1] = tmp
            }
            j++
        }
        i++
    }
    return a
}

arr data = [64, 34, 25, 12, 22, 11, 90]
arr sorted = bubble_sort(data)
sorted[x] > printf "%g ", x
print ""
# => 11 12 22 25 34 64 90
```

---

### Fibonacci

Iterative:

```xf
num fn fib(num n) {
    if n <= 1 { return n }
    num a = 0
    num b = 1
    num i = 2
    while i <= n {
        num tmp = a + b
        a = b
        b = tmp
        i++
    }
    return b
}

num i = 0
i <= 10 <> { printf "fib(%g) = %g\n", i, fib(i); i++ }
```

Recursive:

```xf
num fn fib(num n) {
    if n <= 1 { return n }
    return fib(n - 1) + fib(n - 2)
}
```

---

### Factorial

```xf
num fn factorial(num n) {
    if n <= 1 { return 1 }
    return n * factorial(n - 1)
}

num i = 0
i <= 12 <> { printf "%g! = %g\n", i, factorial(i); i++ }
```

---

### Binary Search

```xf
num fn binary_search(arr a, num target) {
    num lo = 0
    num hi = a.len - 1
    while lo <= hi {
        num mid = int((lo + hi) / 2)
        if a[mid] == target { return mid }
        elif a[mid] < target { lo = mid + 1 }
        else                 { hi = mid - 1 }
    }
    return -1
}

arr data   = [2, 5, 8, 12, 16, 23, 38, 45, 72, 91]
num idx    = binary_search(data, 23)
printf "found 23 at index %g\n", idx    # => 5
printf "found 99 at index %g\n", binary_search(data, 99)  # => -1
```

---

### Caesar Cipher

```xf
str fn caesar(str text, num shift) {
    shift = int(shift % 26)
    arr chars = split(text, "")
    str out = ""
    chars[c] > {
        num code = num(c)
        if code >= 65 && code <= 90 {
            out ..= str((code - 65 + shift) % 26 + 65)
        } elif code >= 97 && code <= 122 {
            out ..= str((code - 97 + shift) % 26 + 97)
        } else {
            out ..= c
        }
    }
    return out
}

str msg     = "Hello, World!"
str encoded = caesar(msg, 13)
str decoded = caesar(encoded, 13)
print encoded    # => Uryyb, Jbeyq!
print decoded    # => Hello, World!
```

---

### Word Count (streaming)

Count words, lines, and characters from stdin or a file:

```xf
# xf -f wc.xf file.txt
BEGIN {
    num words = 0
    num chars = 0
}

{
    chars += len($0) + 1       # +1 for stripped newline
    words += NF
}

END {
    printf "%6g %6g %6g\n", NR, words, chars
}
```

---

### Palindrome Check

```xf
num fn is_palindrome(str s) {
    s = core.str.lower(core.str.trim(s))
    num l = 0
    num r = s.len - 1
    while l < r {
        if substr(s, l, 1) != substr(s, r, 1) { return 0 }
        l++
        r--
    }
    return 1
}

arr words = ["racecar", "hello", "level", "world", "madam"]
words[w] > printf "%-10s %s\n", w, (is_palindrome(w) ? "palindrome" : "not palindrome")
```

---

### CSV Parser and Aggregator

Parse a CSV file, group by a column, and sum another:

```xf
# xf -f agg.xf sales.csv
# Input columns: date, region, amount

BEGIN {
    FS = ","
}

NR > 1 {
    totals[$2] += num($3)
}

END {
    print "Region\tTotal"
    keys(totals)[region] > printf "%-12s %.2f\n", region, totals[region]
}
```

---

### Top-N Lines by Field

Print the 5 lines with the highest value in column 2:

```xf
# xf -f topn.xf data.tsv
BEGIN { FS = "\t" }

{
    push(rows, $0)
    push(vals, num($2))
}

END {
    # bubble sort parallel arrays descending
    num n = rows.len
    num i = 0
    while i < n - 1 {
        num j = 0
        while j < n - i - 1 {
            if vals[j] < vals[j+1] {
                num  tv = vals[j];  vals[j] = vals[j+1];  vals[j+1] = tv
                str  tr = rows[j];  rows[j] = rows[j+1];  rows[j+1] = tr
            }
            j++
        }
        i++
    }
    num k = 0
    k < 5 <> { print rows[k]; k++ }
}
```

---

### State-Safe Division

Demonstrates how `NAV` propagation short-circuits a chain of operations:

```xf
num fn safe_div(num a, num b) {
    if b == 0 { return num(NAV) }
    return a / b
}

arr pairs = [[10, 2], [9, 0], [8, 4], [6, 0], [15, 3]]

pairs[p] > {
    num result = safe_div(p[0], p[1])
    if result.state == OK {
        printf "%g / %g = %g\n", p[0], p[1], result
    } else {
        printf "%g / %g = [NAV — division by zero]\n", p[0], p[1]
    }
}
```

---

### Generate and Filter Primes

```xf
num fn is_prime(num n) {
    if n < 2 { return 0 }
    num i = 2
    while i * i <= n {
        if n % i == 0 { return 0 }
        i++
    }
    return 1
}

arr primes = []
num n = 2
n <= 100 <> {
    if is_prime(n) { push(primes, n) }
    n++
}

printf "primes up to 100 (%g found):\n", primes.len
primes[p] > printf "%g ", p
print ""
```

---

### Parallel File Processing

Hash each file independently, then collect results:

```xf
# xf -j 4 -f hash.xf

arr files = core.os.run_lines("find /var/log -name '*.log' -maxdepth 1")

void fn checksum(str path) {
    str sum = core.os.run("md5sum " .. path)
    print sum
}

files[f] > spawn checksum(f)
join
```