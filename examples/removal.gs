import gs.impl

def nat: Impl::nat
def pair
    x:nat
    y:nat
return
    x:nat
    y:nat
    x followedby y
def keep_left  p:pair return p.a:nat
def keep_right p:pair return p.b:nat

def Main
    p1: pair
    p2: pair
    factor: nat
where
    r1 = keep_left p1
    r2 = keep_right p2
    r3 = Impl::add r1 r2
    ret = Impl::mul r3 factor

run Main
