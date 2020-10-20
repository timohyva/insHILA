#include <sstream>
#include<iostream>
#include <string>
#include <math.h>
#include <assert.h>


#include "plumbing/defs.h"
#include "datatypes/cmplx.h"
#include "datatypes/vector.h"
#include "datatypes/sun.h"
#include "datatypes/wilson_vector.h"
#include "plumbing/field.h"
#include "plumbing/inputs.h"

// Direct output to stdout
std::ostream &hila::output = std::cout;

// Define the lattice global variable
lattice_struct my_lattice;
lattice_struct * lattice = & my_lattice;



const int nd[4] = { 64, 40, 6, 16};


inline void checkLatticeSetup(){
	for (int dir = 0; dir < NDIRS; dir++){
        //check that neighbor arrays are allocated
		assert(lattice->neighb[dir]!=nullptr);
        #ifdef CUDA
        assert(lattice->backend_lattice->d_neighb[dir]!=nullptr);
        #endif
	}
    for(int dir = 0; dir < NDIM; dir++){
    	assert(lattice->size(dir)==nd[dir]);
    }
}

inline void test_setup(int &argc, char **argv){
    #if NDIM==1
    lattice->setup( nd[0], argc, argv );
    #elif NDIM==2
    lattice->setup( nd[0], nd[1], argc, argv );
    #elif NDIM==3
    lattice->setup( nd[0], nd[1], nd[2], argc, argv );
    #elif NDIM==4
    lattice->setup( nd[0], nd[1], nd[2], nd[3], argc, argv );
    #endif
    checkLatticeSetup();

    seed_random(1);
}