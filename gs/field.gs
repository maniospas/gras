universe Field

def num

def merge_add
    a: num
    b: num
    c: num
    result_add1: num
    result_add2: num
    add1: "add"
    add2: "add"
    a arg add1
    b arg add1
    result_add1 arg add2
    c arg add2
    add1 returns result_add1
    add2 returns result_add2
return
    a: num
    b: num
    c: num
    result_add2: num
    a arg add2
    b arg add2
    c arg add2
    add2: "add"
    add2 returns result_add2

def optimization_addsub
    a: num
    b: num
    adds: "add"
    subs: "sub"
    result_add: num
    result_sub: num
    adds returns result_add
    subs returns result_sub
    a arg adds
    b arg adds
    result_add arg0 subs
    b arg1 subs
// just merge a and the final result
return(a: num, result_sub: num)
where a = result_sub
