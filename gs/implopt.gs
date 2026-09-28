import impl
universe Implopt uses Impl

def merge_add
    a: nat
    b: nat
    c: nat
    result_add1: nat
    result_add2: nat
    add1: "add"
    add2: "add"
    a arg add1
    b arg add1
    result_add1 arg add2
    c arg add2
    add1 returns result_add1
    add2 returns result_add2
return
    a: nat
    b: nat
    c: nat
    result_add2: nat
    a arg add2
    b arg add2
    c arg add2
    add2: "add"
    add2 returns result_add2

def optimization_addsub
    a: nat
    b: nat
    adds: "add"
    subs: "sub"
    result_add: nat
    result_sub: nat
    adds returns result_add
    subs returns result_sub
    a arg adds
    b arg adds
    result_add arg0 subs
    b arg1 subs
// just merge a and the final result
return(a: nat, result_sub: nat)
where a = result_sub
