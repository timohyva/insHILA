#ifndef __DIRAC_H__
#define __DIRAC_H__

#include "../plumbing/defs.h"
#include "../datatypes/cmplx.h"
#include "../datatypes/general_matrix.h"
#include "../plumbing/field.h"

template<typename mtype, typename vtype>
void dirac_stagggered_alldim(
    const mtype gauge[NDIM],
    const double mass,
    const vtype &v_in,
    vtype &v_out)
{
    static field<double> eta[NDIM]; // The staggered phase
    static bool initialized = false;

    // Initialize the staggered eta field
    if(!initialized){
        foralldir(d){
            onsites(ALL){
                location l = coordinates(X);
                int sumcoord = 0;
                for(int d2=0;d2<d;d2++){
                    sumcoord += l[d];
                }
                if( sumcoord %2 ){
                    eta[d][X] = 1;
                } else {
                    eta[d][X] =-1;
                }
            }
        }
        initialized = true;
    }
    

    // Apply the mass diagonally
    v_out[ALL] = mass * v_in[X];

    foralldir(dir){
        direction odir = opp_dir( (direction)dir );
        direction odir2 = opp_dir( (direction)dir );
        // Positive directions: get the vector and multiply by matrix stored here
        v_out[ALL] += 0.5*eta[dir][X]*v_in[X+dir]*gauge[dir][X];
        // Negative directions: get both form neighbour
        v_out[ALL] -= 0.5*eta[dir][X]*v_in[X+odir]*gauge[dir][X+odir2].conjugate() ;
    }
}


#if (NDIM==4)
/// A staggered Dirac operator with one unrolled loop instead of
/// loops for all directions. Used in benchmarks.
template<typename mtype, typename vtype>
void dirac_stagggered_4dim(
    const mtype gauge[NDIM],
    const double mass,
    const vtype &v_in,
    vtype &v_out)
{
    static field<double> eta[NDIM]; // The staggered phase
    static bool initialized = false;

    // Initialize the staggered eta field
    if(!initialized){
        foralldir(d){
            onsites(ALL){
                location l = coordinates(X);
                int sumcoord = 0;
                for(int d2=0;d2<d;d2++){
                    sumcoord += l[d];
                }
                if( sumcoord %2 ){
                    eta[d][X] = 1;
                } else {
                    eta[d][X] =-1;
                }
            }
        }
        initialized = true;
    }
    

    // Apply the mass diagonally
    onsites(ALL){
      v_out[X] = mass * v_in[X];
      v_out[X] += 0.5*eta[XUP][X]*v_in[X+XUP]*gauge[XUP][X];
      v_out[X] += 0.5*eta[YUP][X]*v_in[X+YUP]*gauge[YUP][X];
      v_out[X] += 0.5*eta[ZUP][X]*v_in[X+ZUP]*gauge[ZUP][X];
      v_out[X] += 0.5*eta[TUP][X]*v_in[X+TUP]*gauge[TUP][X];
      v_out[X] -= 0.5*eta[XUP][X]*v_in[X+XDOWN]*gauge[XUP][X+XDOWN].conjugate();
      v_out[X] -= 0.5*eta[YUP][X]*v_in[X+YDOWN]*gauge[YUP][X+YDOWN].conjugate();
      v_out[X] -= 0.5*eta[ZUP][X]*v_in[X+ZDOWN]*gauge[ZUP][X+ZDOWN].conjugate();
      v_out[X] -= 0.5*eta[TUP][X]*v_in[X+TDOWN]*gauge[TUP][X+TDOWN].conjugate();
    }
}
#endif


#if NDIM==4
template<typename mtype, typename vtype>
void dirac_stagggered(
    const mtype gauge[NDIM],
    const double mass,
    const vtype &v_in,
    vtype &v_out)
{
    dirac_stagggered_4dim(gauge, mass, v_in, v_out);
}

#else

template<typename mtype, typename vtype>
void dirac_stagggered(
    const mtype gauge[NDIM],
    const double mass,
    const vtype &v_in,
    vtype &v_out)
{
    dirac_stagggered_alldim(gauge, mass, v_in, v_out);
}

#endif


#endif