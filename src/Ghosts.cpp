//=============================================================================
//  Ghosts.cpp
//  Contains all routines for searching for and creating ghost particles.
//  Also contains routine to correct particle positions/velocities to keep 
//  them contained in simulation bounding box.
//
//  This file is part of GANDALF :
//  Graphical Astrophysics code for N-body Dynamics And Lagrangian Fluids
//  https://github.com/gandalfcode/gandalf
//  Contact : gandalfcode@gmail.com
//
//  Copyright (C) 2013  D. A. Hubber, G. Rosotti
//
//  GANDALF is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 2 of the License, or
//  (at your option) any later version.
//
//  GANDALF is distributed in the hope that it will be useful, but
//  WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  General Public License (http://www.gnu.org/licenses) for more details.
//=============================================================================


#include <cstdlib>
#include <math.h>
#include <map>
#include <string>
#include "Precision.h"
#include "Constants.h"
#include "Debug.h"
#include "Exception.h"
#include "Simulation.h"
#include "SphParticle.h"
#include "Sph.h"
#include "Ghosts.h"
using namespace std;



//=============================================================================
//  Ghosts::CheckBoundaries
/// Check all particles to see if any have crossed the simulation bounding 
/// box.  If so, then move the particles to their new location on the other 
/// side of the periodic box.
//=============================================================================
template <int ndim>
void PeriodicGhosts<ndim>::CheckBoundaries
(DomainBox<ndim> simbox,
 Sph<ndim> *sph)
{
  int i;                            // Particle counter
  SphParticle<ndim> *part;          // Pointer to SPH particle data
  SphIntParticle<ndim> *partint;    // Pointer to SPH integration data


  // Loop over all particles and check if any lie outside the periodic box.
  // If so, then re-position with periodic wrapping.
  //---------------------------------------------------------------------------
#pragma omp parallel for default(none) private(i,part) shared(simbox,sph)
  for (i=0; i<sph->Nsph; i++) {
    part = &sph->sphdata[i];
    partint = &sph->sphintdata[i];

    if (part->r[0] < simbox.boxmin[0])
      if (simbox.x_boundary_lhs == "periodic") {
        part->r[0] += simbox.boxsize[0];
        partint->r0[0] += simbox.boxsize[0];
      }
    if (part->r[0] > simbox.boxmax[0])
      if (simbox.x_boundary_rhs == "periodic") {
        part->r[0] -= simbox.boxsize[0];
        partint->r0[0] -= simbox.boxsize[0];
      }

    if (ndim >= 2 && part->r[1] < simbox.boxmin[1])
      if (simbox.y_boundary_lhs == "periodic") {
        part->r[1] += simbox.boxsize[1];
        partint->r0[1] += simbox.boxsize[1];
      }
    if (ndim >= 2 && part->r[1] > simbox.boxmax[1])
      if (simbox.y_boundary_rhs == "periodic") {
        part->r[1] -= simbox.boxsize[1];
        partint->r0[1] -= simbox.boxsize[1];
      }

    if (ndim == 3 && part->r[2] < simbox.boxmin[2])
      if (simbox.z_boundary_lhs == "periodic") {
        part->r[2] += simbox.boxsize[2];
        partint->r0[2] += simbox.boxsize[2];
      }
    if (ndim == 3 && part->r[2] > simbox.boxmax[2])
      if (simbox.z_boundary_rhs == "periodic") {
        part->r[2] -= simbox.boxsize[2];
        partint->r0[2] -= simbox.boxsize[2];
      }

  }
  //---------------------------------------------------------------------------

  return;
}



//=============================================================================
//  Ghosts::SearchGhostParticles
/// Search domain to create any required ghost particles near any boundaries.
/// Currently only searches to create periodic or mirror ghost particles.
//=============================================================================
template <int ndim>
void PeriodicGhosts<ndim>::SearchGhostParticles
(FLOAT tghost,                      ///< Ghost particle 'lifetime'
 DomainBox<ndim> simbox,            ///< Simulation box structure
 Sph<ndim> *sph)                    ///< Sph object pointer
{
  int i;                                                // Particle counter
  FLOAT kernrange = sph->kernp->kernrange*sph->kernfac; // Kernel extent
  SphParticle<ndim>* sphdata = sph->sphdata;            // SPH particle data

  // Set all relevant particle counters
  sph->Nghost    = 0;
  sph->NPeriodicGhost = 0;
  sph->Nghostmax = sph->Nsphmax - sph->Nsph;
  sph->Ntot      = sph->Nsph;

  // If all boundaries are open, immediately return to main loop
  if (simbox.x_boundary_lhs == "open" && simbox.x_boundary_rhs == "open" &&
      simbox.y_boundary_lhs == "open" && simbox.y_boundary_rhs == "open" &&
      simbox.z_boundary_lhs == "open" && simbox.z_boundary_rhs == "open")
    return;

  debug2("[SphSimulation::SearchGhostParticles]");

  // Create ghost particles in x-dimension
  //---------------------------------------------------------------------------
  if ((simbox.x_boundary_lhs == "open" && 
       simbox.x_boundary_rhs == "open") == 0) {
    for (i=0; i<sph->Ntot; i++) {
      if (sphdata[i].r[0] + min(0.0,sphdata[i].v[0]*tghost) <
          simbox.boxmin[0] + ghost_range*kernrange*sphdata[i].h) {
        if (simbox.x_boundary_lhs == "periodic")
          CreateGhostParticle(i,0,sphdata[i].r[0] + simbox.boxsize[0],
                              sphdata[i].v[0],sph,x_lhs_periodic);
        if (simbox.x_boundary_lhs == "mirror")
          CreateGhostParticle(i,0,2.0*simbox.boxmin[0] - sphdata[i].r[0],
                              -sphdata[i].v[0],sph,x_lhs_mirror);
      }
      if (sphdata[i].r[0] + max(0.0,sphdata[i].v[0]*tghost) >
          simbox.boxmax[0] - ghost_range*kernrange*sphdata[i].h) {
        if (simbox.x_boundary_rhs == "periodic")
          CreateGhostParticle(i,0,sphdata[i].r[0] - simbox.boxsize[0],
                              sphdata[i].v[0],sph,x_rhs_periodic);
        if (simbox.x_boundary_rhs == "mirror")
          CreateGhostParticle(i,0,2.0*simbox.boxmax[0] - sphdata[i].r[0],
                              -sphdata[i].v[0],sph,x_rhs_mirror);
      }
    }
    sph->Ntot = sph->Nsph + sph->Nghost;
  }


  // Create ghost particles in y-dimension
  //---------------------------------------------------------------------------
  if (ndim >= 2 && (simbox.y_boundary_lhs == "open" && 
		    simbox.y_boundary_rhs == "open") == 0) {
    for (i=0; i<sph->Ntot; i++) {
      if (sphdata[i].r[1] + min(0.0,sphdata[i].v[1]*tghost) <
          simbox.boxmin[1] + ghost_range*kernrange*sphdata[i].h) {
        if (simbox.y_boundary_lhs == "periodic")
          CreateGhostParticle(i,1,sphdata[i].r[1] + simbox.boxsize[1],
                              sphdata[i].v[1],sph,y_lhs_periodic);
	    if (simbox.y_boundary_lhs == "mirror")
          CreateGhostParticle(i,1,2.0*simbox.boxmin[1] - sphdata[i].r[1],
                              -sphdata[i].v[1],sph,y_lhs_mirror);
      }
      if (sphdata[i].r[1] + max(0.0,sphdata[i].v[1]*tghost) >
          simbox.boxmax[1] - ghost_range*kernrange*sphdata[i].h) {
        if (simbox.y_boundary_rhs == "periodic")
          CreateGhostParticle(i,1,sphdata[i].r[1] - simbox.boxsize[1],
                              sphdata[i].v[1],sph,y_rhs_periodic);
        if (simbox.y_boundary_rhs == "mirror")
          CreateGhostParticle(i,1,2.0*simbox.boxmax[1] - sphdata[i].r[1],
                              -sphdata[i].v[1],sph,y_rhs_mirror);
      }
    }
    sph->Ntot = sph->Nsph + sph->Nghost;
  }


  // Create ghost particles in z-dimension
  //---------------------------------------------------------------------------
  if (ndim == 3 && (simbox.z_boundary_lhs == "open" && 
		    simbox.z_boundary_rhs == "open") == 0) {
    for (i=0; i<sph->Ntot; i++) {
      if (sphdata[i].r[2] + min(0.0,sphdata[i].v[2]*tghost) <
          simbox.boxmin[2] + ghost_range*kernrange*sphdata[i].h) {
        if (simbox.z_boundary_lhs == "periodic")
          CreateGhostParticle(i,2,sphdata[i].r[2] + simbox.boxsize[2],
                              sphdata[i].v[2],sph,z_lhs_periodic);
        if (simbox.z_boundary_lhs == "mirror")
          CreateGhostParticle(i,2,2.0*simbox.boxmin[2] - sphdata[i].r[2],
                              -sphdata[i].v[2],sph,z_lhs_mirror);
      }
      if (sphdata[i].r[2] + max(0.0,sphdata[i].v[2]*tghost) >
          simbox.boxmax[2] - ghost_range*kernrange*sphdata[i].h) {
        if (simbox.z_boundary_rhs == "periodic")
          CreateGhostParticle(i,2,sphdata[i].r[2] - simbox.boxsize[2],
                              sphdata[i].v[2],sph,z_rhs_periodic);
        if (simbox.z_boundary_rhs == "mirror")
          CreateGhostParticle(i,2,2.0*simbox.boxmax[2] - sphdata[i].r[2],
                              -sphdata[i].v[2],sph,z_rhs_mirror);
      }
    }
    sph->Ntot = sph->Nsph + sph->Nghost;
  }

  // Quit here if we've run out of memory for ghosts
  if (sph->Ntot > sph->Nsphmax) {
    string message="Not enough memory for ghost particles";
    ExceptionHandler::getIstance().raise(message);
  }

  sph->NPeriodicGhost = sph->Nghost;

  //cout << "NGHOST : " << sph->Nghost << "     " << sph->Nghostmax << endl;

  return;
}



//=============================================================================
//  Ghosts::CreateGhostParticle
/// Create a new ghost particle from either 
/// (i) a real SPH particle (i < Nsph), or 
/// (ii) an existing ghost particle (i >= Nsph).
//=============================================================================
template <int ndim>
void PeriodicGhosts<ndim>::CreateGhostParticle
(int i,                             ///< [in] i.d. of original particle
 int k,                             ///< [in] Boundary dimension for new ghost
 FLOAT rk,                          ///< [in] k-position of original particle
 FLOAT vk,                          ///< [in] k-velocity of original particle
 Sph<ndim> *sph,                    ///< [inout] SPH particle object pointer
 int ghosttype)                     ///< ..
{
  // Increase ghost counter and check there's enough space in memory
  if (sph->Nghost > sph->Nghostmax) {
    string message= "Not enough memory for new ghost";
    ExceptionHandler::getIstance().raise(message);
  }

  // If there's enough memory, create ghost particle in arrays
  sph->sphdata[sph->Nsph + sph->Nghost] = sph->sphdata[i];
  sph->sphdata[sph->Nsph + sph->Nghost].r[k] = rk;
  sph->sphdata[sph->Nsph + sph->Nghost].v[k] = vk;
  sph->sphdata[sph->Nsph + sph->Nghost].active = false;
  sph->sphdata[sph->Nsph + sph->Nghost].itype = ghosttype;

  // Record id of original particle for later copying
  //if (i >= sph->Nsph)
  //  sph->sphdata[sph->Nsph + sph->Nghost].iorig = sph->sphdata[i].iorig;
  //else
  //  sph->sphdata[sph->Nsph + sph->Nghost].iorig = i;
  sph->sphdata[sph->Nsph + sph->Nghost].iorig = i;

  sph->Nghost = sph->Nghost + 1;

  return;
}



//=============================================================================
//  Ghosts::CopySphDataToGhosts
/// Copy any newly calculated data from original SPH particles to ghosts.
//=============================================================================
template <int ndim>
void PeriodicGhosts<ndim>::CopySphDataToGhosts
(DomainBox<ndim> simbox,
 Sph<ndim> *sph)
{
  int i;                            // Particle id
  int iorig;                        // Original (real) particle id
  int itype;                        // Ghost particle type
  int j;                            // Ghost particle counter
  int k;                            // Dimension counter
  FLOAT rp[ndim];                   // Particle position
  FLOAT vp[ndim];                   // Particle velocity

  debug2("[SphSimulation::CopySphDataToGhosts]");

  //---------------------------------------------------------------------------
//#pragma omp parallel for default(none) private(i,iorig,itype,j,k) shared(simbox,sph)
  for (j=0; j<sph->NPeriodicGhost; j++) {
    i = sph->Nsph + j;
    iorig = sph->sphdata[i].iorig;
    itype = sph->sphdata[i].itype;

    //for (k=0; k<ndim; k++) rp[k] = sph->sphdata[i].r[k];
    //for (k=0; k<ndim; k++) vp[k] = sph->sphdata[i].v[k];
    
    sph->sphdata[i] = sph->sphdata[iorig];
    sph->sphdata[i].iorig = iorig;
    sph->sphdata[i].itype = itype;
    sph->sphdata[i].active = false;
    //for (k=0; k<ndim; k++) sph->sphdata[i].r[k] = rp[k];
    //for (k=0; k<ndim; k++) sph->sphdata[i].v[k] = vp[k];

    // Modify ghost position based on ghost type
    if (itype == x_lhs_periodic) sph->sphdata[i].r[0] += simbox.boxsize[0];
    else if (itype == x_rhs_periodic) sph->sphdata[i].r[0] -= simbox.boxsize[0];
    else if (itype == y_lhs_periodic) sph->sphdata[i].r[1] += simbox.boxsize[1];
    else if (itype == y_rhs_periodic) sph->sphdata[i].r[1] -= simbox.boxsize[1];
    
  }
  //---------------------------------------------------------------------------

  return;
}



template <int ndim>
void NullGhosts<ndim>::CheckBoundaries(DomainBox<ndim> simbox, Sph<ndim> *sph)
{
  return;
}



template <int ndim>
void NullGhosts<ndim>::SearchGhostParticles
(FLOAT tghost,                      ///< Ghost particle 'lifetime'
 DomainBox<ndim> simbox,            ///< Simulation box structure
 Sph<ndim> *sph)                    ///< Sph object pointer
{

  // Set all relevant particle counters
  sph->Nghost    = 0;
  sph->NPeriodicGhost = 0;
  sph->Nghostmax = sph->Nsphmax - sph->Nsph;
  sph->Ntot      = sph->Nsph;

 return;
}



template <int ndim>
void NullGhosts<ndim>::CopySphDataToGhosts(DomainBox<ndim> simbox, Sph<ndim> *sph) {
  return;
}



#if defined MPI_PARALLEL
template <int ndim>
void MPIGhosts<ndim>::CheckBoundaries(DomainBox<ndim> simbox, Sph<ndim> *sph)
{
  return;
}



//=============================================================================
//  MPIGhosts::SearchGhostParticles
/// Handle control to MpiControl to compute particles to send to other nodes
/// and receive from them, then copy received ghost particles inside the main arrays
//=============================================================================
template <int ndim>
void MPIGhosts<ndim>::SearchGhostParticles
(FLOAT tghost,                      ///< Ghost particle 'lifetime'
 DomainBox<ndim> simbox,            ///< Simulation box structure
 Sph<ndim> *sph)                    ///< Sph object pointer
{
  int i;
  int j;
  SphParticle<ndim>* ghost_array;
  int Nmpighosts = mpicontrol->SendReceiveGhosts(&ghost_array, sph);

  if (sph->Ntot + Nmpighosts > sph->Nsphmax) {
    cout << "Error: not enough memory for MPI ghosts!!! " << Nmpighosts << " " << sph->Ntot << " " << sph->Nsphmax<<endl;
    ExceptionHandler::getIstance().raise("");
  }

  SphParticle<ndim>* main_array = sph->sphdata;
  int start_index = sph->Nsph + sph->NPeriodicGhost;

  for (j=0; j<Nmpighosts; j++) {
    i = start_index + j;
    main_array[i] =  ghost_array[j];
    main_array[i].active = false;
  }

  sph->Nghost += Nmpighosts;
  sph->Ntot += Nmpighosts;

  if (sph->Nghost > sph->Nghostmax || sph->Ntot > sph->Nsphmax) {
	cout << "Error: not enough memory for MPI ghosts!!! " << Nmpighosts << " " << sph->Ntot << " " << sph->Nsphmax<<endl;
	ExceptionHandler::getIstance().raise("");
  }

}


template <int ndim>
void MPIGhosts<ndim>::CopySphDataToGhosts(DomainBox<ndim> simbox, Sph<ndim> *sph) {

  SphParticle<ndim>* ghost_array;
  int Nmpighosts = mpicontrol->UpdateGhostParticles(&ghost_array);
  SphParticle<ndim>* main_array = sph->sphdata;
  int start_index = sph->Nsph + sph->NPeriodicGhost;

  for (int j=0; j<Nmpighosts; j++) {
    int i = start_index + j;
    main_array[i] =  ghost_array[j];
    main_array[i].active = false;
  }

}
#endif



// Create template class instances of the main SphSimulation object for
// each dimension used (1, 2 and 3)
template class NullGhosts<1>;
template class NullGhosts<2>;
template class NullGhosts<3>;
template class PeriodicGhosts<1>;
template class PeriodicGhosts<2>;
template class PeriodicGhosts<3>;
#ifdef MPI_PARALLEL
template class MPIGhosts<1>;
template class MPIGhosts<2>;
template class MPIGhosts<3>;
#endif
