import gs.impl

def nat: Impl::nat
def pair
    x:nat
    y:nat
return
    x:nat
    y:nat
    x followedby y

def Main
    p1: pair
where
    r3 = Impl::add p1 // the difficulty is that the 'followedby' edge is different

run Main