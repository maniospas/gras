import gs.impl
import gs.implopt

def Main(x: nat, y: nat, x followedby y)
where
    r = Impl::add x y
    ret = Impl::sub r y
    reduce all Implopt::merge_add
    reduce all Implopt::optimization_addsub

run Main
