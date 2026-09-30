import gs.impl

universe Array
uses Impl
def narray
def farray

//  Constructors
def toarr(a:nat)
return (call:"narray",a:nat,result:narray,call returns result,a arg call)

def toarr(a:float)
return (call:"farray",a:float,result:farray,call returns result,a arg call)

//  Casts
def tonat(a:farray)
return (call:"narray",a:farray,result:narray,call returns result,a arg call)

def tofloat(a:narray)
return (call:"farray",a:narray,result:farray,call returns result,a arg call)

//  Element access
def get(a:narray,i:nat)
return (call:"nget",a:narray,i:nat,result:nat,call returns result,a arg0 call,i arg1 call)

def get(a:farray,i:nat)
return (call:"fget",a:farray,i:nat,result:float,call returns result,a arg0 call,i arg1 call)

//  Slices
def slice(a:narray,start:nat,size:nat)
return (
    call:"nslice",
    a:narray,
    start:nat,
    size:nat,
    result:narray,
    call returns result,
    a arg0 call,
    start arg1 call,
    size arg2 call
)

def slice(a:farray,start:nat,size:nat)
return (
    call:"fslice",
    a:farray,
    start:nat,
    size:nat,
    result:farray,
    call returns result,
    a arg0 call,
    start arg1 call,
    size arg2 call
)


//  Addition
def add(a:narray,b:narray)
return (
    call:"map",
    fn:"nadd",
    a:narray,
    b:narray,
    result:narray,
    call returns result,
    fn arg0 call,
    a arg1 call,
    b arg2 call
)


def add(a:farray,b:farray)
return (
    call:"map",
    fn:"fadd",
    a:farray,
    b:farray,
    result:farray,
    call returns result,
    fn arg0 call,
    a arg1 call,
    b arg2 call
)


//  Subtraction
def sub(a:narray,b:narray,a followedby b)
return (
    call:"map",
    fn:"nsub",
    a:narray,
    b:narray,
    result:narray,
    call returns result,
    fn arg0 call,
    a arg1 call,
    b arg2 call
)

def sub(a:farray,b:farray,a followedby b)
return (
    call:"map",
    fn:"fsub",
    a:farray,
    b:farray,
    result:farray,
    call returns result,
    fn arg0 call,
    a arg1 call,
    b arg2 call
)


//  Multiplication
def mul(a:narray,b:narray)
return (
    call:"map",
    fn:"nmul",
    a:narray,
    b:narray,
    result:narray,
    call returns result,
    fn arg0 call,
    a arg1 call,
    b arg2 call
)

def mul(a:farray,b:farray)
return (
    call:"map",
    fn:"fmul",
    a:farray,
    b:farray,
    result:farray,
    call returns result,
    fn arg0 call,
    a arg1 call,
    b arg2 call
)


//  Division
def div(a:narray,b:narray,a followedby b)
return (
    call:"map",
    fn:"ndiv",
    a:narray,
    b:narray,
    result:narray,
    call returns result,
    fn arg0 call,
    a arg1 call,
    b arg2 call
)

def div(a:farray,b:farray,a followedby b)
return (
    call:"map",
    fn:"fdiv",
    a:farray,
    b:farray,
    result:farray,
    call returns result,
    fn arg0 call,
    a arg1 call,
    b arg2 call
)


//  Modulo
def mod(a:narray,b:narray,a followedby b)
return (
    call:"map",
    fn:"nmod",
    a:narray,
    b:narray,
    result:narray,
    call returns result,
    fn arg0 call,
    a arg1 call,
    b arg2 call
)


//  Power
def pow(a:narray,b:narray,a followedby b)
return (
    call:"map",
    fn:"npow",
    a:narray,
    b:narray,
    result:narray,
    call returns result,
    fn arg0 call,
    a arg1 call,
    b arg2 call
)

def pow(a:farray,b:farray,a followedby b)
return (
    call:"map",
    fn:"fpow",
    a:farray,
    b:farray,
    result:farray,
    call returns result,
    fn arg0 call,
    a arg1 call,
    b arg2 call
)


//  Minimum
def min(a:narray,b:narray)
return (
    call:"map",
    fn:"nmin",
    a:narray,
    b:narray,
    result:narray,
    call returns result,
    fn arg0 call,
    a arg1 call,
    b arg2 call
)

def min(a:farray,b:farray)
return (
    call:"map",
    fn:"fmin",
    a:farray,
    b:farray,
    result:farray,
    call returns result,
    fn arg0 call,
    a arg1 call,
    b arg2 call
)


//  Maximum
def max(a:narray,b:narray)
return (
    call:"map",
    fn:"nmax",
    a:narray,
    b:narray,
    result:narray,
    call returns result,
    fn arg0 call,
    a arg1 call,
    b arg2 call
)

def max(a:farray,b:farray)
return (
    call:"map",
    fn:"fmax",
    a:farray,
    b:farray,
    result:farray,
    call returns result,
    fn arg0 call,
    a arg1 call,
    b arg2 call
)


//  Reductions
def sum(a:narray)
return (
    call:"reduce",
    fn:"nadd",
    a:narray,
    result:nat,
    call returns result,
    fn arg0 call,
    a arg1 call
)

def sum(a:farray)
return (
    call:"reduce",
    fn:"fadd",
    a:farray,
    result:float,
    call returns result,
    fn arg0 call,
    a arg1 call
)

def product(a:narray)
return (
    call:"reduce",
    fn:"nmul",
    a:narray,
    result:nat,
    call returns result,
    fn arg0 call,
    a arg1 call
)

def product(a:farray)
return (
    call:"reduce",
    fn:"fmul",
    a:farray,
    result:float,
    call returns result,
    fn arg0 call,
    a arg1 call
)

def min(a:narray)
return (
    call:"reduce",
    fn:"nmin",
    a:narray,
    result:nat,
    call returns result,
    fn arg0 call,
    a arg1 call
)

def min(a:farray)
return (
    call:"reduce",
    fn:"fmin",
    a:farray,
    result:float,
    call returns result,
    fn arg0 call,
    a arg1 call
)

def max(a:narray)
return (
    call:"reduce",
    fn:"nmax",
    a:narray,
    result:nat,
    call returns result,
    fn arg0 call,
    a arg1 call
)

def max(a:farray)
return (
    call:"reduce",
    fn:"fmax",
    a:farray,
    result:float,
    call returns result,
    fn arg0 call,
    a arg1 call
)


//  Concatenation
def cat(a:narray,b:narray,a followedby b)
return (
    call:"narray",
    a:narray,
    b:narray,
    result:narray,
    call returns result,
    a arg0 call,
    b arg1 call
)

def cat(a:farray,b:farray,a followedby b)
return (
    call:"farray",
    a:farray,
    b:farray,
    result:farray,
    call returns result,
    a arg0 call,
    b arg1 call
)

// Map

def fmap(fn:string, lines:sarray)
return (
    close,
    call:"map",
    result:farray,
    call returns result,
    fn arg0 call,
    lines arg1 call
)
