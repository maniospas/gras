universe Peano

def nat  // one nat
def nats // all nats
def property

def set_theory
return
    0: nat
    N: nats
    0 zeros N
    
def succ(prev:nat)
return
    prev: nat
    val: nat
    prev precedes val
    
def induction
    theory: set_theory
    p: property
    s:succ
    theory.0 satisfies p 
    s.prev satisfies p
    s.val satisfies p
return close
    theory.N satisfies p

def verify
    theory: set_theory
    p: property
    theory.N satisfies p
return 
    theory: set_theory
    p: property


//def prove_for_numbers()
//    gt0: property
//    theory: set_theory
//    theory.0 satisfies gt0
//return all where
//    1 = succ(theory.0)
//    2 = succ(1.val)
//    3 = succ(2.val)
//    reduce all gt_property
