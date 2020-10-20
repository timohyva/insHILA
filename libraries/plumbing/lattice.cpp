


#include "plumbing/globals.h"
#include "plumbing/lattice.h"
#include "plumbing/field.h"


//HACK: force disable vectorization in a loop using
// if(disable_avx[X]==0){};
field<double> disable_avx;

///***********************************************************
/// setup() lays out the lattice infrastruct, with neighbour arrays etc.

/// A list of all defined lattices
std::vector<lattice_struct*> lattices;


/// General lattice setup, including MPI setup
void lattice_struct::setup(int siz[NDIM], int &argc, char **argv) {
  // Add this lattice to the list 
  lattices.push_back( this );

  l_volume = 1;
  for (int i=0; i<NDIM; i++) {
    l_size[i] = siz[i];
    l_volume *= siz[i];
  }

#ifdef USE_MPI
  /* Initialize MPI */
  initialize_machine(argc, &argv);

  /* default comm is the world */
  mpi_comm_lat = MPI_COMM_WORLD;

  MPI_Comm_rank( lattices[0]->mpi_comm_lat, &this_node.rank );
  MPI_Comm_size( lattices[0]->mpi_comm_lat, &nodes.number );

#else 
  this_node.rank = 0;
  nodes.number = 1;
#endif

  setup_layout();

  setup_nodes();

  // set up the comm arrays 
  create_std_gathers();

  // Initialize wait_array structures - has to be after std gathers()
  initialize_wait_arrays();

#ifdef SPECIAL_BOUNDARY_CONDITIONS
  // do this after std. boundary is done
  init_special_boundaries();
#endif

#ifndef VANILLA
  /* Setup backend-specific lattice info if necessary */
  backend_lattice = new backend_lattice_struct;
  backend_lattice->setup(*this);
#endif

  test_std_gathers();

  disable_avx = 0;

}


#if NDIM==4
void lattice_struct::setup(int nx, int ny, int nz, int nt, int &argc, char **argv) {
  int s[NDIM] = {nx, ny, nz, nt};
  setup(s, argc, argv);
}
#elif NDIM==3
void lattice_struct::setup(int nx, int ny, int nz, int &argc, char **argv) {
  int s[NDIM] = {nx, ny, nz};
  setup(s, argc, argv);
}
#elif NDIM==2
void lattice_struct::setup(int nx, int ny, int &argc, char **argv) {
  int s[NDIM] = {nx, ny};
  setup(s, argc, argv);
}
#elif NDIM==1
void lattice_struct::setup(int nx, int &argc, char **argv) {
  int s[NDIM] = {nx};
  setup(s, argc, argv);
}
#endif

///////////////////////////////////////////////////////////////////////
/// The routines is_on_node(), node_rank(), site_index()
/// implement the "layout" of the nodes and sites of the lattice.
/// To be changed in different implementations!
///////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////
/// Get the node rank for coordinates 
/// This is the fundamental routine which defines how the nodes
/// are mapped.  map_node_layout MUST BE compatible with this
/// algorithm!  So if you change this, change that too
/// 
/// Here the node number along one direction is calculated with
///    loc * n_divisions/l_size  (with integer division)
/// example: with l_size=14 and n_divisions=3,
/// the dividers are at 0, 5, 10, 14
/// 
///////////////////////////////////////////////////////////////////////

int lattice_struct::node_rank(const coordinate_vector & loc)
{
  int i;
  int dir;

  i = (loc[NDIM-1] * nodes.n_divisions[NDIM-1]) / l_size[NDIM-1];
  for (dir=NDIM-2; dir>=0; dir--) {
    i = i*nodes.n_divisions[dir] +
        ((loc[dir] * nodes.n_divisions[dir]) / l_size[dir]);
  }
  /* do we want to remap this?  YES PLEASE */
  i = nodes.remap(i);

  return( i );
}

///////////////////////////////////////////////////////////////////////
/// Is the coordinate on THIS node 
///////////////////////////////////////////////////////////////////////

bool lattice_struct::is_on_node(const coordinate_vector & loc)
{
  int d;

  for (int dir=0; dir<NDIM; dir++) {
    d = loc[dir] - this_node.min[dir];
    if (d < 0 || d >= this_node.size[dir] ) return false;
  }
  return true;
}


///////////////////////////////////////////////////////////////////////
/// give site index for ON NODE sites
/// Note: loc really has to be on this node
///////////////////////////////////////////////////////////////////////

#ifndef SUBNODE_LAYOUT

unsigned lattice_struct::site_index(const coordinate_vector & loc)
{
  int dir,l,s;
  unsigned i;

  i = l = loc[NDIM-1] - this_node.min[NDIM-1];
  s = loc[NDIM-1];
  for (dir=NDIM-2; dir>=0; dir--) {
    l = loc[dir] - this_node.min[dir];
    i = i*this_node.size[dir] + l;
    s += loc[dir];
  }

  // now i contains the `running index' for site
#if defined(EVEN_SITES_FIRST)
  if (s%2 == 0) return( i/2 );    /* even site index */
  else return( i/2 + this_node.evensites );  /* odd site */
#else
  return( i );
#endif
}

///////////////////////////////////////////////////////////////////////
/// give site index for nodeid sites
/// compare to above
///////////////////////////////////////////////////////////////////////

unsigned lattice_struct::site_index(const coordinate_vector & loc, const unsigned nodeid)
{
  int dir,l,s;
  unsigned i;
  const node_info & ni = nodes.nodelist[nodeid];
  
  i = l = loc[NDIM-1] - ni.min[NDIM-1];
  s = loc[NDIM-1];
  for (dir=NDIM-2; dir>=0; dir--) {
    l = loc[dir] - ni.min[dir];
    i = i*ni.size[dir] + l;
    s += loc[dir];
  }

  // now i contains the `running index' for site
#if defined(EVEN_SITES_FIRST)
  if (s%2 == 0) return( i/2 );    /* even site index */
  else return( i/2 + ni.evensites );  /* odd site */
#else
  return( i );
#endif
}



#else  // now SUBNODE_LAYOUT

///////////////////////////////////////////////////////////////////////
/// The (AVX) vectorized version of the site_index function.
/// Now there are two stages: an "inner" vect index, which goes over
/// over "virtual nodes", and the "outer" index inside the virtual node.
/// This is to enable the (float/32bit) and the (double/64bit) vectorized structures,
/// where the latter is achieve by merging two of the 32bit subnodes, 
/// along the direction merged_subnodes_dir
/// E.g. 
/// a 2-dim. 4x8 node is divided into 8 / 4 subnodes as follows:  
/// here 0-3 / 0-7 is the index within subnode, and a-h / a-d the subnode label.
///
///     32bit(float)   64bit(double)
///                                             32bit storage: 64bit storage:    
///    0a 1a | 0b 1b   0a 2a | 0b 2b            0(abcdefgh)    0(abcd) 1(abcd)
///    2a 3a | 2b 3b   4a 6a | 4b 6b            1(abcdefgh)    2(abcd) 3(abcd)
///    -------------                            2(abcdefgh)    4(abcd) 5(abcd)
///    0e 1e | 0f 1f   1a 3a | 1b 3b            3(abcdefgh)    6(abcd) 7(abcd)
///    2e 3e | 2f 3f   5a 7a | 5b 7b          
///    -------------   -------------           32bit vectors 1st half <-> even 64bit vectors
///    0c 1c | 0d 1d   0c 2c | 0d 2d           32bit 2nd half <-> odd 64bit
///    2c 3c | 2d 3d   4c 6c | 4d 6d         
///    -------------                           The "storage" order above is the site-by-site
///    0g 1g | 0h 1h   1c 3c | 1d 3d           order, enabling site traversal with maximum locality.
///    2g 3g | 2h 3h   5c 7c | 5d 7d           It also enables mixed 32bit/64bit vector algebra with half vectors.
/// 
/// Direction where the "doubling" is done is the last direction where subnodes are divided
/// In layout, this will become the "slowest" direction
///////////////////////////////////////////////////////////////////////

unsigned lattice_struct::site_index(const coordinate_vector & loc)
{
  return site_index(loc, mynode());
}

///////////////////////////////////////////////////////////////////////
/// give site index for nodeid sites
/// compare to above
///////////////////////////////////////////////////////////////////////

unsigned lattice_struct::site_index(const coordinate_vector & loc, const unsigned nodeid)
{
  int dir,l,s,subl;
  unsigned i;

  assert (nodeid < nodes.number);
  const node_info & ni = nodes.nodelist[nodeid];
  
  // let's mod the coordinate to sublattice
  // get subnode size - divisons are the same in all nodes

  // foralldir(d) assert( ni.size[d] % this_node.subnodes.divisions[d] == 0);

  coordinate_vector subsize = ni.size/this_node.subnodes.divisions;

  dir = this_node.subnodes.merged_subnodes_dir;
  l = loc[dir] - ni.min[dir];
  subl = l / subsize[dir];
  // flip to interleaved ordering, see above  --  this should work 
  // automatically w. mapping between 32 and 64 bit vector elems
  subl = subl/2 + (subl % 2)*(this_node.subnodes.divisions[dir]/2);

  i = 0;
  s = 0;

  for (dir=NDIM-1; dir>=0; dir--) {
    l = loc[dir] - ni.min[dir];
    if (dir != this_node.subnodes.merged_subnodes_dir) { 
      subl = subl * this_node.subnodes.divisions[dir] 
           + l / subsize[dir];
    }
    i = i * subsize[dir] + l % subsize[dir];
    s += loc[dir];
  }

#if defined(EVEN_SITES_FIRST)
  if (s%2 == 0) i = i/2;              /* even site index */
  else i = i/2 + ni.evensites/number_of_subnodes;  /* odd site */
#endif

  return (subl + number_of_subnodes*i);
}

#endif // SUBNODE_LAYOUT

///////////////////////////////////////////////////////////////////////
/// invert the this_node index -> location (only on this node)
///////////////////////////////////////////////////////////////////////

// this is defined in lattice.h


/////////////////////////////////////////////////////////////////////
/// Set up the basic list about all nodes
/// NOTE: in case of very large number of nodes this may be
/// inconveniently large.  Alternatives?
/////////////////////////////////////////////////////////////////////

void lattice_struct::setup_nodes() {

  nodes.number = numnodes();
  // Loop over all node "origins"
  nodes.nodelist.resize(nodes.number);

  // n keeps track of the node "root coordinates"
  coordinate_vector n(0);

  // use nodes.divisors - vectors to fill in stuff
  for (int i=0; i<nodes.number; i++) {
    coordinate_vector l;
    foralldir(d) l[d] = nodes.divisors[d][n[d]];

    int nn = node_rank(l);
    node_info & ni = nodes.nodelist[nn];
    int v = 1;
    foralldir(d) {
      ni.min[d]  = nodes.divisors[d][n[d]];
      ni.size[d] = nodes.divisors[d][n[d]+1] - nodes.divisors[d][n[d]];
      v *= ni.size[d];
    }
    if (v % 2 == 0)
      ni.evensites = ni.oddsites = v/2;
    else {
      // now node ni has odd number of sites
      if (l.parity() == EVEN) {
        ni.evensites = v/2 + 1; 
        ni.oddsites = v/2;
      } else {
        ni.evensites = v/2; 
        ni.oddsites = v/2 + 1;
      }
    }
    
    // now to the next divisor - add to the lowest dir until all done
    foralldir(d) {
      n[d]++;
      if (n[d] < nodes.n_divisions[d]) break;
      n[d] = 0;
    }
    
    // use the opportunity to set up this_node when it is met
    if (nn == mynode())
      this_node.setup(ni, *lattice);
  }
}


////////////////////////////////////////////////////////////////////////
/// Fill in this_node fields -- node_rank() must be set up OK
////////////////////////////////////////////////////////////////////////
void lattice_struct::node_struct::setup(node_info & ni, lattice_struct & lattice)
{
  
  rank = mynode();

  min  = ni.min;
  size = ni.size;

  evensites = ni.evensites;
  oddsites  = ni.oddsites;
  sites     = ni.evensites + ni.oddsites;

  first_site_even = (min.parity() == EVEN);
   
  // neighbour node indices
  foralldir(d) {
    coordinate_vector l = min;   // this is here on purpose
    l[d] = (min[d] + size[d]) % lattice.l_size[d];
    nn[d] = lattice.node_rank(l);
    l[d] = (lattice.l_size[d] + min[d] - 1) % lattice.l_size[d];
    nn[opp_dir(d)] = lattice.node_rank(l);
  }

  // map site indexes to locations -- coordinates array
  // after the above site_index should work

  coordinates.resize(sites);
  coordinate_vector l = min;
  for(unsigned i = 0; i<sites; i++){
    coordinates[ lattice.site_index(l) ] = l;
    // walk through the coordinates
    foralldir(d) {
      if (++l[d] < (min[d]+size[d])) break;
      l[d] = min[d];
    }
  }

#ifdef SUBNODE_LAYOUT
  
  // set up the subnodes
  subnodes.setup(*this);
#endif

}

#ifdef SUBNODE_LAYOUT

////////////////////////////////////////////////////////////////////////
/// Fill in subnodes -struct
////////////////////////////////////////////////////////////////////////
void lattice_struct::node_struct::subnode_struct::setup(const node_struct & tn)
{
  size = tn.size / divisions;
  evensites = tn.evensites / number_of_subnodes;
  oddsites  = tn.oddsites / number_of_subnodes;
  sites     = evensites + oddsites;

}

#endif

/////////////////////////////////////////////////////////////////////
/// Create the neighbour index arrays 
/// This is for the index array neighbours
/// TODO: implement some other neighbour schemas!
/////////////////////////////////////////////////////////////////////

void lattice_struct::create_std_gathers()
{

  // allocate neighbour arrays - TODO: these should 
  // be allocated on "device" memory too!

  for (int d=0; d<NDIRS; d++) {
    neighb[d] = (unsigned *)memalloc(this_node.sites * sizeof(unsigned));
  }
  
  unsigned c_offset = this_node.sites;  // current offset in field-arrays

#ifdef SCHROED_FUN
  // special case for Schroedinger functional: fixed boundary
  // This requires we allocate one extra space for
  // CONSTANT schroedinger b.c.
  // Thus, add 1 item to latfield arrays to
  // take the boundary into account, and set the
  // neighb[d] to point to it.  This is for mpi or vanilla.
  // NOTE: IF NON-CONSTANT BOUNDARY NEED TO ADD c_offset FOR
  // EACH SITE, and set the corresponding neighb

  int sf_special_boundary = c_offset;
  /* add space for boundary only when needed */
  if (this_node.min[NDIM-1] + this_node.nodesize[NDIM-1] == size(NDIM-1])
    c_offset += 1;
  else sf_special_boundary = -(1<<30);

  output0 << "SPECIAL BOUNDARY LAYOUT for SF";
#endif


  // We set the communication and the neigbour-array here

  for (direction d=XUP; d<NDIRS; ++d) {

    nn_comminfo[d].index = neighb[d];    // this is not really used for nn gathers

    comm_node_struct & from_node = nn_comminfo[d].from_node;
    // we can do the opposite send during another pass of the sites. 
    // This is just the gather inverted
    // NOTE: this is not the send to direction d, but to -d!    
    comm_node_struct & to_node = nn_comminfo[-d].to_node;

    from_node.rank = to_node.rank = this_node.rank;    // invalidate from_node, for time being
    // if there are no communications the rank is left as is

    // counters to zero
    from_node.sites = from_node.evensites = from_node.oddsites = 0;

    // pass over sites
    int num = 0;  // number of sites off node
    for (int i=0; i<this_node.sites; i++) {
      coordinate_vector ln, l;
      l = coordinates(i);
      // set ln to be the neighbour of the site
      // TODO: FIXED BOUNDARY CONDITIONS DO NOT WRAP
      ln = mod(l + d, size());
      //ln = l;
      //if (is_up_dir(d)) ln[d] = (l[d] + 1) % size(d);
      //else ln[d] = (l[d] + size(-d) - 1) % size(-d);
 
#ifdef SCHROED_FUN
      if (d == NDIM-1 && l[NDIM-1] == size(NDIM-1)-1) {
	      // This is up-direction, give special site
    	  neighb[d][i] = sf_special_boundary;
      } else if (d == opp_dir(NDIM-1) && l[NDIM-1] == 0) {
	      //  We never should need the down direction, thus, short circuit!
	      neighb[d][i] = 1<<30;
      } else     // NOTE THIS UGLY else-if CONSTRUCT!
#endif
      if (is_on_node(ln)) {
        neighb[d][i] = site_index(ln);
      } else {
        // reset neighb array temporarily, as a flag
        neighb[d][i] = this_node.sites;

	      // Now site is off-node, this leads to fetching
        // check that there's really only 1 node to talk with
        unsigned rank = node_rank(ln);
        if ( from_node.rank == this_node.rank) {
          from_node.rank = rank;
        } else if (from_node.rank != rank) {
          hila::output << "Internal error in nn-communication setup\n";
          exit(-1);
        }

        from_node.sites++;
        if (l.parity() == EVEN) from_node.evensites++;
        else from_node.oddsites++;

	      num++;
      }
    }

    // and set buffer indices
    from_node.buffer = c_offset;

    to_node.rank = from_node.rank;
    to_node.sites = from_node.sites;
    // note!  Parity is flipped, because parity is normalized to receieving node
    to_node.evensites = from_node.oddsites;
    to_node.oddsites = from_node.evensites;

    if (num > 0) {
      // sitelist tells us which sites to send
      to_node.sitelist = (unsigned *)memalloc(to_node.sites * sizeof(unsigned));
#ifndef VANILLA
      // non-vanilla code MAY want to have receive buffers, so we need mapping to field
      from_node.sitelist = (unsigned *)memalloc(from_node.sites * sizeof(unsigned));
#endif
    } else 
      to_node.sitelist = nullptr;

    if (num > 0) {
      // set the remaining neighbour array indices and sitelists in another go over sites.
      // temp counters
      // NOTE: ordering is automatically right: with a given parity,
      // neighbour node indices come in ascending order of host node index - no sorting needed
      int c_even, c_odd;
      c_even = c_odd = 0;

      for (int i=0; i<this_node.sites; i++) {
        if (neighb[d][i] == this_node.sites) {
          coordinate_vector ln, l;
          l = coordinates(i);
          ln = mod(l + d, size()); 

          if (l.parity() == EVEN) {
            // THIS site is even
            neighb[d][i] = c_offset + c_even;

#ifndef VANILLA
            from_node.sitelist[c_even] = i;
#endif

            // flipped parity: this is for odd sends
            to_node.sitelist[c_even + to_node.evensites] = i;

            c_even++;

          } else {
            neighb[d][i] = c_offset + from_node.evensites + c_odd;

#ifndef VANILLA
            from_node.sitelist[c_odd + from_node.evensites] = i;
#endif

            // again flipped parity for setup
            to_node.sitelist[c_odd] = i;

            c_odd++;
          }
        }
      }
    }

    c_offset += from_node.sites;

  } /* directions */

  /* Finally, set the site to the final offset (better be right!) */
  this_node.field_alloc_size = c_offset;


}



///  Get message tags cyclically -- defined outside classes, so that it is global and unique

#define MSG_TAG_MIN  10000
#define MSG_TAG_MAX  (1<<28)     // a big int number

int get_next_msg_tag() {
  static int tag = MSG_TAG_MIN;
  ++tag;
  if (tag > MSG_TAG_MAX) tag = MSG_TAG_MIN;
  return tag;
}





/************************************************************************/

#ifdef USE_MPI
/* this formats the wait_array, used by forallsites_waitA()site_neighbour
 * wait_array[i] contains a bit at position 1<<dir if nb(dir,i) is out
 * of lattice.
 * Site OK if ((wait_arr ^ xor_mask ) & and_mask) == 0
 */

static_assert(NDIM <= 4 && 
  "Dimensions at most 4 in dir_mask_t = unsigned char!  Use larger type to circumvent");

void lattice_struct::initialize_wait_arrays()
{
  int i,dir;

  /* Allocate here the mask array needed for forallsites_wait
   * This will contain a bit at location dir if the neighbour
   * at that dir is out of the local volume
   */

  wait_arr_  = (dir_mask_t *)memalloc( this_node.sites * sizeof(unsigned char) );

  for (int i=0; i<this_node.sites; i++) {
    wait_arr_[i] = 0;    /* basic, no wait */
    foralldir(dir) {
      direction odir = -dir;
      if ( neighb[dir][i]  >= this_node.sites ) wait_arr_[i] = wait_arr_[i] | (1<<dir) ;
      if ( neighb[odir][i] >= this_node.sites ) wait_arr_[i] = wait_arr_[i] | (1<<odir) ;
    }
  }
}

#else

void lattice_struct::initialize_wait_arrays(){}

#endif

#ifdef SPECIAL_BOUNDARY_CONDITIONS

/////////////////////////////////////////////////////////////////////
/// set up special boundary
/// sets up the bool array which tells if special neighbour indices
/// are needed.  Note that this is not uniform across the nodes,
/// not all nodes have them.
/////////////////////////////////////////////////////////////////////

void lattice_struct::init_special_boundaries() {
  for (direction d=(direction)0; d<NDIRS; ++d) {

    // default values, nothing interesting happens
    special_boundaries[d].n_even = special_boundaries[d].n_odd = 
      special_boundaries[d].n_total = 0;
    special_boundaries[d].is_needed = false;
    special_boundaries[d].is_on_edge = false;

    direction od = -d;
    int coord = -1;
    // do we get up/down boundary?
    if (is_up_dir(d) && this_node.min[d] + this_node.size[d] == size(d)) coord = size(d)-1;
    if (is_up_dir(od) && this_node.min[od] == 0) coord = 0;

    if (coord >= 0) {
      // now we got it
      special_boundaries[d].is_on_edge = true;

      if (nodes.n_divisions[abs(d)] == 1) {
        special_boundaries[d].is_needed = true;
        special_boundaries[d].offset = this_node.field_alloc_size;

        for (int i=0; i<this_node.sites; i++) if (coordinate(abs(d),i) == coord) {
          // set buffer indices
          special_boundaries[d].n_total++;
          if (site_parity(i) == EVEN) special_boundaries[d].n_even++;
          else special_boundaries[d].n_odd++;
        }
        this_node.field_alloc_size += special_boundaries[d].n_total;
      }

    }

    // hila::output << "Node " << mynode() << " dir " << d << " min " << this_node.min << " is_on_edge "
    //   << special_boundaries[d].is_on_edge << '\n';

    // allocate neighbours only on 1st use, otherwise unneeded
    special_boundaries[d].neighbours = nullptr;    
  }
}

/////////////////////////////////////////////////////////////////////
/// give the neighbour array pointer.  Allocate if needed

const unsigned * lattice_struct::get_neighbour_array(direction d, boundary_condition_t bc) {

  // regular bc exit, should happen almost always
  if (special_boundaries[d].is_needed == false ||
      bc == boundary_condition_t::PERIODIC) return neighb[d];

  if (special_boundaries[d].neighbours == nullptr) {
    setup_special_boundary_array(d);
  }
  return special_boundaries[d].neighbours;
}

//////////////////////////////////////////////////////////////////////
/// and set up the boundary to one direction
//////////////////////////////////////////////////////////////////////

void lattice_struct::setup_special_boundary_array(direction d) {
  // if it is not needed or already done...
  if (special_boundaries[d].is_needed == false ||
      special_boundaries[d].neighbours != nullptr) return;

  // now allocate neighbour array and the fetching array
  special_boundaries[d].neighbours = (unsigned *)memalloc(sizeof(unsigned) * this_node.sites);
  special_boundaries[d].move_index = (unsigned *)memalloc(sizeof(unsigned) * special_boundaries[d].n_total);

  int coord;
  int offs = special_boundaries[d].offset;
  if (is_up_dir(d)) coord = size(d)-1; else coord = 0;

  int k = 0;
  for (int i=0; i<this_node.sites; i++) {
    if (coordinate(abs(d),i) != coord) {
      special_boundaries[d].neighbours[i] = neighb[d][i];
    } else {
      special_boundaries[d].neighbours[i] = offs++;
      special_boundaries[d].move_index[k++] = neighb[d][i];
    }
  }

  assert( k == special_boundaries[d].n_total );
}

#endif

/////////////////////////////////////////////////////////////////////
/// Create the neighbour index arrays 
/// This is for the index array neighbours
/// TODO: implement some other neighbour schemas!
/////////////////////////////////////////////////////////////////////

#if 1

/// This is a helper routine, returning a vector of comm_node_structs for all nodes
/// involved with communication. 
/// If receive == true, this is "receive" end and index will be filled.
/// For receive == false the is "send" half is done.

std::vector<lattice_struct::comm_node_struct> 
lattice_struct::create_comm_node_vector( coordinate_vector offset, unsigned * index, bool receive) {

  // for send flip the offset
  if (!receive) offset = -offset;

  // temp work array: np = node points
  std::vector<unsigned> np_even(nodes.number);   // std::vector initializes to zero
  std::vector<unsigned> np_odd(nodes.number);

  // we'll go through the sites twice, in order to first resolve the size of the needed
  // buffers, then to fill them.  Trying to avoid memory fragmentation a bit

  // pass over sites
  int num = 0;  // number of sites off node
  for (int i=0; i<this_node.sites; i++) {
    coordinate_vector ln, l;
    l  = coordinates(i);
    ln = mod( l + offset, size() );
 
    if (is_on_node(ln)) {
      if (receive) index[i] = site_index(ln);
    } else {
	    // Now site is off-node, this will leads to fetching
      // use ci.index to store the node rank
      unsigned r = node_rank(ln);

      if (receive) {
        index[i] = this_node.sites + r;

        // using parity of THIS
        if (l.parity() == EVEN) np_even[r]++;
        else np_odd[r]++;

      } else {
        // on the sending side - we use parity of target
        if (ln.parity() == EVEN) np_even[r]++;  
        else np_odd[r]++;
      }

      num++;
    }
  }

  // count the number of nodes taking part
  unsigned nnodes = 0;
  for (int r=0; r<nodes.number; r++) {
    if (np_even[r] > 0 || np_odd[r] > 0) nnodes++;
  }

  // allocate the vector
  std::vector<comm_node_struct> node_v(nnodes);

  int n = 0;
  int c_buffer = 0;
  for (int r = 0; r<nodes.number; r++) {
    if (np_even[r] > 0 || np_odd[r] > 0) {
      // add the rank
      node_v[n].rank = r;
      node_v[n].evensites = np_even[r];
      node_v[n].oddsites  = np_odd[r];
      node_v[n].sites = np_even[r] + np_odd[r];

      // pre-allocate the sitelist for sufficient size
      if (!receive) node_v[n].sitelist = (unsigned *)memalloc(node_v[n].sites * sizeof(unsigned));

      node_v[n].buffer = c_buffer;        // running idx to comm buffer - used from receive
      c_buffer += node_v[n].sites;
      n++;
    }
  }

  // ci.receive_buf_size = c_buffer;  // total buf size

  // we'll reuse np_even and np_odd as counting arrays below
  for (int i=0; i<nnodes; i++) np_even[i] = np_odd[i] = 0;

  if (!receive) {
    // sending end -- create sitelists

    for (int i=0; i<this_node.sites; i++) {
      coordinate_vector ln, l;
      l  = coordinates(i);
      ln = mod( l + offset, size() );
 
      if (!is_on_node(ln)) {
        unsigned r = node_rank(ln);
        int n = 0;
        // find the node from the list 
        while (node_v[n].rank != r) n++;
        // we'll fill the buffers according to the parity of receieving node
        // first even, then odd sites in the buffer
        int k;
        if (ln.parity() == EVEN) k = np_even[n]++;
        else k = node_v[n].evensites + np_odd[n]++;

        // and set the ptr to the site to be communicated
        node_v[n].sitelist[k] = i;        
      }
    }

  } else {
    // receive end
    // fill in the index pointers

    for (int i=0; i<this_node.sites; i++) {
      if (index[i] >= this_node.sites) {
        int r = index[i] - this_node.sites;
        int n = 0;
        // find the node which sends this 
        while (node_v[n].rank != r) n++;

        coordinate_vector l = coordinates(i);
        if (l.parity() == EVEN)
          index[i] = node_v[n].buffer + (np_even[n]++);
        else 
          index[i] = node_v[n].buffer + node_v[n].evensites + (np_odd[n]++);
      }
    }
  }

  return node_v;
}




lattice_struct::gen_comminfo_struct lattice_struct::create_general_gather( const coordinate_vector & offset )
{
  // allocate neighbour arrays - TODO: these should 
  // be allocated on "device" memory too!
    
  gen_comminfo_struct ci;

  // communication buffer
  ci.index = (unsigned *)memalloc(this_node.sites*sizeof(unsigned));

  ci.from_node = create_comm_node_vector(offset, ci.index, true);  // create receive end
  ci.to_node   = create_comm_node_vector(offset, nullptr, false);  // create sending end

  // set the total receive buffer size from the last vector
  const comm_node_struct & r = ci.from_node[ci.from_node.size()-1];
  ci.receive_buf_size = r.buffer + r.sites;

  return ci;
}

#endif


