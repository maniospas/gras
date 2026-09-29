import gs.peano
uses Peano

def gt_property
    x: nat
    y: nat
    gt: property
    x satisfies gt
    x precedes y
return close // close copies all inputs to returns
    y satisfies gt

def prove_forall()
    gt0: property
    theory: set_theory
    when:succ
    theory.0  satisfies gt0
    when.prev satisfies gt0
return all where
    // we automatically apply transformations
    reduce all induction|gt_property
    // verify that theroy.N satisfies gt0 (error otherwise)
    reduce verify(theory, gt0) 
