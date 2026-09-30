universe Impl

def nat
def float
def string
def sarray

def add(a:nat,b:nat)
return (
    call:"nadd",
    a:nat,
    b:nat,
    result:nat,
    call returns result,
    a arg0 call,
    b arg1 call
)

def sub(a:nat,b:nat,a followedby b)
return (
    call:"nsub",
    a:nat,
    b:nat,
    result:nat,
    call returns result,
    a arg0 call,
    b arg1 call
)

def mul(a:nat,b:nat)
return (
    call:"nmul",
    a:nat,
    b:nat,
    result:nat,
    call returns result,
    a arg0 call,
    b arg1 call
)

def div(a:nat,b:nat,a followedby b)
return (
    call:"ndiv",
    a:nat,
    b:nat,
    result:nat,
    call returns result,
    a arg0 call,
    b arg1 call
)

def mod(a:nat,b:nat,a followedby b)
return (
    call:"nmod",
    a:nat,
    b:nat,
    result:nat,
    call returns result,
    a arg0 call,
    b arg1 call
)

def pow(a:nat,b:nat,a followedby b)
return (
    call:"npow",
    a:nat,
    b:nat,
    result:nat,
    call returns result,
    a arg0 call,
    b arg1 call
)

def min(a:nat,b:nat)
return (
    call:"nmin",
    a:nat,
    b:nat,
    result:nat,
    call returns result,
    a arg0 call,
    b arg1 call
)

def max(a:nat,b:nat)
return (
    call:"nmax",
    a:nat,
    b:nat,
    result:nat,
    call returns result,
    a arg0 call,
    b arg1 call
)


def add(a:float,b:float)
return (
    call:"fadd",
    a:float,
    b:float,
    result:float,
    call returns result,
    a arg0 call,
    b arg1 call
)

def sub(a:float,b:float,a followedby b)
return (
    call:"fsub",
    a:float,
    b:float,
    result:float,
    call returns result,
    a arg0 call,
    b arg1 call
)

def mul(a:float,b:float)
return (
    call:"fmul",
    a:float,
    b:float,
    result:float,
    call returns result,
    a arg0 call,
    b arg1 call
)

def div(a:float,b:float,a followedby b)
return (
    call:"fdiv",
    a:float,
    b:float,
    result:float,
    call returns result,
    a arg0 call,
    b arg1 call
)

def slice(a:string,start:nat,size:nat)
return (
    call:"sslice",
    a:string,
    start:nat,
    size:nat,
    result:string,
    call returns result,
    a arg0 call,
    start arg1 call,
    size arg2 call
)

def pow(a:float,b:float,a followedby b)
return (
    call:"fpow",
    a:float,
    b:float,
    result:float,
    call returns result,
    a arg0 call,
    b arg1 call
)

def min(a:float,b:float)
return (
    call:"fmin",
    a:float,
    b:float,
    result:float,
    call returns result,
    a arg0 call,
    b arg1 call
)

def max(a:float,b:float)
return (
    call:"fmax",
    a:float,
    b:float,
    result:float,
    call returns result,
    a arg0 call,
    b arg1 call
)

def tonat(a:float)
return (
    call:"ncast",
    a:float,
    result:nat,
    call returns result,
    a arg call
)

def tofloat(a:nat)
return (
    call:"fcast",
    a:nat,
    result:float,
    call returns result,
    a arg call
)



//  String / numeric conversions

def tonat(a:string)
return (
    call:"ston",
    a:string,
    result:nat,
    call returns result,
    a arg call
)

def tofloat(a:string)
return (
    call:"stof",
    a:string,
    result:float,
    call returns result,
    a arg call
)

def tostring(a:nat)
return (
    call:"ntos",
    a:nat,
    result:string,
    call returns result,
    a arg call
)

def tostring(a:float)
return (
    call:"ftos",
    a:float,
    result:string,
    call returns result,
    a arg call
)


//  String operations

def cat(a:string,b:string,a followedby b)
return (
    call:"scat",
    a:string,
    b:string,
    result:string,
    call returns result,
    a arg0 call,
    b arg1 call
)

def get(a:string,i:nat)
return (
    call:"nget",
    a:string,
    i:nat,
    result:nat,
    call returns result,
    a arg0 call,
    i arg1 call
)

def slice(a:string,start:nat,size:nat)
return (
    call:"sslice",
    a:string,
    start:nat,
    size:nat,
    result:string,
    call returns result,
    a arg0 call,
    start arg1 call,
    size arg2 call
)

def split(a:string,separator:string,a followedby separator)
return (
    call:"ssplit",
    a:string,
    separator:string,
    result:sarray,
    call returns result,
    a arg0 call,
    separator arg1 call
)