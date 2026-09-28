# GSlang

*A language for graph substitutions.*

**Author: Emmanouil Krasanakis**

*Disclaimer: The code has mostly been written by ChatGPT, with manual review, refinement, and testing. This is a re-implementation of the most significan parts of my PhD thesis some years after its conclusion. he rewrite helps frame obscure theoretical results and tools in simpler terms.*


GS is a language for proofs using knowledge graph 
substitutions; variables are nodes, and named relations 
are labeled directed edges. There are subistitution rules 
for "proving" programs, where the proof may also translate
to creating a runable pipeline. Do note that the proofs
are logically consistent despite allowing abstract textual
predicates in their definitions.

## Universes

A universe is a namespace whose types can be accessed
via `::`. You can skip the namespace access if already
inside it. Below is an example, which also shows
how import can port namespaces from other *.gs* files.
By default, your file is in the `Global` namespace.

```python
import gs.impl // contains the Impl namespace too

universe Myuniverse
def nat: Impl::nat
```


## Types

A def declares a graph. Commas and parentheses are optional,
but do prefer them when annotating in one line. The example
declares two fields *x,y*, which would be unpacked into their
sub-components if they were of more complicated type. But here
they are just of a base def `nat`. The last def argument 
is how to create a labeled directed relation. Use `!` in front
of the label to prevents a graph matching when the corresponding 
relations exist.

```python
import gs.impl
def nat: impl::nat // redeclare in this universe
def pair(x: nat, y: nat, x followedby y)
```

## Return Graphs

`return` turns a def a function and declares its returned graph.

```python
def keep_left(p: pair)
return
    p.a: nat
```

The returned graph is separate from the input graph. Relations 
declared only in `return` belong only to the return graph. 
Can use parentheses and commas similarly to before too. At the
same time, the returned graph constitutes the function's type,
for example to reference types via the names of their constructors.

**Returned node are NOT automatically preserved from the input graph.** That is, you will need to redeclare nodes that you want to keep.
Relations are not automatically preserved either, but this rarely
matters in envisioned use cases.

There is also a `where` block for types, in which def inference
and input-output wiring can take place. For example, this is
valid:

```python
def keep_left(p: pair)
return ret: nat
where ret=p.x
```


## Where

`where` merges nodes between inputs and/or outputs, and
uses functions to transform the input graph. It comprises
several statements that may remove variable nodes by
making graph subsitutions via functions. THe result may
be stored on newly created or returned variables. Here is
an example, where `run main` just lets us run the type
as a program. 

Remaining inputs (plus new ones gnerated)
become runtime inputs. Function calls are graph
substitution operations, where the nodes of the
replacing subgraph (the one that has just been
used to replace the subgraph matching function
inputs) are stored on one variable. The result
is then unified with the assigned name, to be
easily referenced.

```python
def Main(x: nat, y: nat)
where
    r = add x y
    ret = sub r y
run main
```

Functions substitutes subgraphs with one of the same node types
and a compatible subset of edges. Positive relations are reinstated
afterwords, whereas newlly disconnected graph components are 
removed.

Parentheses create temporary results automatically. The next example
is equivalent to creating a temporary for `add x y` and passing
that result to `mul`. Use `reduce` instead of `ret=` to not track
the return with a variable. Newlines and commas end expressions
(unless within a parenthesis.)

```python
ret = mul (add x y) factor
```

`reduce all` automatiically searches and performs all
available function applications.

```python
import gs.impl

universe Impl // work within Imp
def main(x: nat, y: nat, x followedby y)
where
    r = add x y
    ret = sub r y
    reduce all optimization_addsub

run main
```

A function can merge nodes through the following pattern:

```python
def assert_equal(a: nat, b:nat)
return(a: nat, b: nat)
where a = b
```

## Builtin functions

*Unorganized concepts about creating new builtin functions.*

A function is a def with a return graph.
String types such as `"add"` identify implementation call nodes.

```python
def add(a: nat, b: nat)
return
    call: "add"
    a: nat
    b: nat
    result: nat
    call returns result
    a arg call
    b arg call
```

`arg` relations represent a variadic argument route.
Use `arg0`, `arg1`, etc. for fixed-position arguments.
Calls implement either of those schema.

``` text
a arg0 call
b arg1 call
```


## Keywords

The language recognizes these words as standalone keywords.

``` text
universe
type
return
where
reduce
all
run
import
```

## Example

``` python
import gs.impl

def nat: Impl::nat
def pair(x: nat, y: nat, x followedby y)
def keep_left(p: pair) return p.a: nat

def main(p1: pair, p2: pair, factor: nat)
where
    left = keep_left p1
    right = keep_left p2
    sum = Impl::add left right
    result = Impl::mul sum factor

run main
```
