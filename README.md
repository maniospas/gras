# GraS

*A language for graph substitutions.*

**Author: Emmanouil Krasanakis**

*Disclaimer: The code has mostly been written by ChatGPT, with manual review, refinement, and testing. This is a re-framing of the most significant theory of my PhD some years after its conclusion to a concrete language.*


GraS is a language for proofs using labeled graph
substitutions; variables are nodes, and named relations 
are labeled directed edges. There are substitution rules 
for "proving" programs, where the proof may also translate
to creating a runnable pipeline. Do note that the proofs
are logically consistent despite allowing abstract textual
predicates in their definitions.

## 🚀 Quickstart

Clone this repository and run  `python gs.py --serve` to 
open the interactive environment in your browser.
Start writing your program.

![examples/preview.png](examples/preview.png)

## 📚 Short guide

### Universes

A universe is a namespace that collects several types.
Its types can be accessed via `::` or via the `uses`
syntax seen below. You can skip special syntax for accessing
types of the universe currently being extended. Below is an example, 
which also shows how import can port namespaces from other *.gs* files.
By default, your file starts in the *Global* universe. If you
want to perform type inference that creates runnable programs
use the *Impl* universe. Universes can be mixed freely within
types, for example to mix different proof systems or implementations
with theoretical analysis.

```python
import gs.impl

universe Myuniverse 
def nat: Impl::nat  // port to this universe
```

Code within the currently parsed universe to access
the declarations of one or more others. To avoid the `::`
syntax prefer the following pattern; that pattern does
does *not* expose *Impl* through the new universe.

```python
import gs.impl
uses Impl // local usage

universe Myuniverse uses Impl // changing universes resets uses
```


### Types

A type refers to a graph of variables with relations between them.
Commas and parentheses are optional here so that you can either
use an one-line function calling syntax or list elements one
under the other. The example
declares two fields *x,y*, where fields would be unpacked into their
sub-components if they were of more complicated type. But here
they are just of a base type `nat`. The last argument, which does
not contain the `:` but is of the form `field1 label field2`
creates a labeled directed relation between fields
(these transfer between all field elements). Use `!` in front
of the label to indicate a negative edge; that is, that this
edge should explicitly not be allowed during partial graph matching
checks later.

```python
import gs.impl
uses Impl

def pair(x: nat, y: nat, x followedby y)
```

### Return

`return` turns a type into a function by declaring its returned graph.
More details later, but importantly any inputs not found in the 
outputs are DROPPED. Variables unpack to their type's
internals immediately. That is, `p: pair` unpacks into 
`p.x: nat, p.y: nat, p.x followedby p.y`. Conversely, could use
`p` to also gather all variables starting with `p.`. This intentionally
looks and behaves similarly to structure access of other languages.

```python
import gs.impl
uses Impl

def pair(x: nat, y: nat, x followedby y)
def keep_left(p: pair)
return p.x: nat
```

The returned graph is separate from the input graph. Relations 
declared only in `return` belong only to the return graph, although
variables with the exact same name carry over for easier use.
Parentheses and commas can be used similarly to before too. The returned graph
constitutes the function's type, for example to reference types via the names
of their constructors. *Referencing the type of the substitution is not supported yet.*

**Returned nodes are NOT automatically preserved from the input graph.** 
That is, you will need to redeclare nodes that you want to keep.
Relations are not automatically preserved either, but this rarely
matters in envisioned use cases.

There is also a `where` block, in which type inference,
reductions, and input-output wiring can take place. 
For example, this is valid as a very precise counterpart
to the previous example:

```python
import gs.impl
uses Impl

def pair(x: nat, y: nat, x followedby y)
def keep_left(p: pair)
return ret: nat
where ret=p.x
```

Returns are not alike the returns of most other languages 
(including imperative and functional languages) in that they
are essentially graph transformations. However, they can
often feel like normal returns when using the `return all`
syntax exemplified below.
Line changes or spaces are not significant in GraS, so 
this organization of keywords is just a conceptual pattern.

```python
import gs.impl
uses Impl

def addpairs(x:nat,y:nat,z:nat,x followedby y, y followedby z)
return all where
    r1 = add(x,y)
    r2 = add(y,z)
```

To clarify on what the above snippet does, encountering `all`
within a `return` asks the graph substitution function to 
put on hold the rest of the return declaration, 
parse a `where` that is now permitted to create new variables,
and add to returns all variables. Then it continues with the 
rest of the return, which is important for declaring relations
on variables that are not seen yet but will be created later.

Importantly,`where` segments
are not allowed to create new output relations, as those
could mix inputs and outputs. For this reason, the rest of the
return is parsed afterward, injecting output variable relations.
Here is a variation of the above example, where the relation 
`r1 followedby r2` is added to the output.

```python
import gs.impl
uses Impl

def addpairs(x:nat,y:nat,z:nat,x followedby y, y followedby z)
return 
    all 
    r1 followedby r2
where
    r1 = add(x,y)
    r2 = add(y,z)

run addpairs
```


### Where

`where` has two functions:
- differently-named variable nodes between inputs and/or outputs via equality
- uses functions to transform the output graph

The `return all where` pattern we previously saw hides away
just how restrictive the above two conditions, so it requires 
stressing that, when defining very precise graph transformations,
you need to first declare ALL variables in returns, including
temporaries, and then perform only graph substitutions (that look 
like calls) between them. Direct assignment between inputs and
outputs is the other mode for adding substitution details, which
again requires that the outputs are already declared; though 
same-named variables are preserved between inputs and outputs.

Graph substitutions look like function calls and take the form
of several statements that may remove variable nodes by
making graph substitutions via functions. The result may
be stored on temporary or returned variables (use `reduce` instead 
of `var=` to store a result in a temporary name, mainly when
applying transformations that simplify graphs). Here is
an example, where `run main` just runs the type as a program;
**running as a program is possible only when using types of the Impl universe, which know how to correctly hook into the runnable interface.**

Remaining inputs (plus new ones generated)
become runtime inputs. Function calls are graph
substitution operations, where the nodes of the
replacing subgraph (the one that has just been
used to replace the subgraph matching function
inputs) are stored on one variable. The result
is then unified with the assigned name, to be
easily referenced.

```python
import gs.impl
uses Impl

def main(x: nat, y: nat)
return all where
    r = add(x,y)
    ret = sub(r,y)
    
run main
```

It has been already mentioned that this calling-like notation
substitutes subgraphs with one of the same node types
and a compatible subset of edges. This is the exact mechanism:
1. Match the nodes in the program's graph.
2. Match a subset of edges of the induced program subgraph for matched nodes.
3. Replace the induced subgraph with the substitution rule's outcome. This will partially match some inputs with some outputs, but may remove some nodes too (nodes and edges are consumed linearly by rule application).
4. Remove all dangling disconnected subgraphs due to removed nodes.
5. Reinstate relations between output nodes that are isomorphic to those of the original induced subgraph but have *not* been considered by the input graph.

The next expression
is equivalent to creating a temporary variable for `add(x,y)` and passing
that result to `mul`. Use `reduce` instead of `ret=` to not track
the return with a variable. Newlines and commas end expressions
(unless within a parenthesis.)

```python
ret = mul(add(x,y), factor)
```

By the way, you can add relations manually to be consumed immediately
but the substitution's inputs. Here is an example, where notice how `addpairs`
is called to add structure in arguments; the order does not matter unless
a (partial) order is granted through appropriately-labeled relations.

```python
import gs.impl
uses Impl

def addpairs(x:nat,y:nat,z:nat,x followedby y, y followedby z)
return all r1 followedby r2 where
    r1 = add(x,y)
    r2 = add(y,z)

def main(x:nat, y:nat, z:nat)
return all where
    ret = sub(addpairs(x,y,z, x followedby y, y followedby z))

run main
```

Something to keep in mind is that **GraS consumes substitution inputs, but these may be reinstated by the operation**.
For example, the *Impl* universe's arithmetic operations preserve nodes and only
add calling nodes with argument relations to the inputs.
In cases where `return all where` is used, all used inputs are reinstated as part
of the output graph!


Finally, `reduce all` automatically searches and performs all
available function applications. This could be computationally
unbounded in some scenarios, but safety checks will be added
in the future.

```python
import gs.impl
import gs.implopt
uses Impl
uses Implopt

def main(x: nat, y: nat, z: nat)
where
    ret = sub(add(x,y,x followedby y), y) // x+y-y
    reduce all merge_add
    reduce all optimization_addsub

run Main
```

It has already been mentioned, but functions can merge nodes through the equality syntax 
between variables; always keep in mind that equality in GraS is a wiring operation!

```python
def assert_equal(a: nat, b:nat)
return(a: nat, b: nat)
where a = b
```

### Builtins

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


### Keyword summary

The language recognizes the following keywords.

```text
import
universe
uses
def
return
all
where
run
reduce
```
