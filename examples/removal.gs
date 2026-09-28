import gs.impl
uses Impl

def pair
    x:nat
    y:nat
return
    x:nat
    y:nat
    x followedby y
def keep_left(p:pair) return p.a:nat
def keep_right(p:pair) return p.b:nat

def main
    x: nat
    y: nat
    p: pair
    factor: nat
return all where
    r3 = Impl::add(keep_left(x,y,x followedby y), keep_right(p))
    ret = Impl::mul(r3,factor)

run main
