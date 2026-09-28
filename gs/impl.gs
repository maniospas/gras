universe Impl
def nat

def add(a: nat, b: nat)
return
    call: "add"
    a: nat
    b: nat
    result: nat
    call returns result
    a arg call
    b arg call

def mul(a: nat, b: nat)
return
    call: "mul"
    a: nat
    b: nat
    result: nat
    call returns result
    a arg call
    b arg call

def max(a: nat, b: nat)
return
    call: "max"
    a: nat
    b: nat
    result: nat
    call returns result
    a arg call
    b arg call

def min(a: nat, b: nat)
return
    call: "min"
    a: nat
    b: nat
    result: nat
    call returns result
    a arg call
    b arg call

def sub(a: nat, b: nat, a followedby b)
return
    call: "sub"
    a: nat
    b: nat
    result: nat
    call returns result
    a arg0 call
    b arg1 call

def div(a: nat, b: nat, a followedby b)
return
    call: "div"
    a: nat
    b: nat
    result: nat
    call returns result
    a arg0 call
    b arg1 call
