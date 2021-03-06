//=============================================================================
//  MpiControl.cpp
//  Contains functions for Main MPI class which controls the distribution of 
//  work amongst all MPI tasks for the current simulation, including load 
//  balancing and moving and copying particles between nodes.
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


#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <math.h>
#include <numeric>
#include "Constants.h"
#include "Precision.h"
#include "SphKernel.h"
#include "DomainBox.h"
#include "Diagnostics.h"
#include "Debug.h"
#include "Exception.h"
#include "InlineFuncs.h"
#include "MpiControl.h"
#include "MpiTree.h"
using namespace std;



//=============================================================================
//  MpiControl::MpiControl()
/// MPI control class constructor.  Initialises all MPI control variables, 
/// plus calls MPI routines to find out rank and number of processors.
//=============================================================================
template <int ndim>
MpiControl<ndim>::MpiControl()
{
  int len;                          // Length of host processor name string
  Box<ndim> dummy;                  // Dummy box variable

  allocated_mpi = false;
  balance_level = 0;

  // Find local processor rank, total no. of processors and host processor name
  MPI_Comm_size(MPI_COMM_WORLD,&Nmpi);
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);
  MPI_Get_processor_name(hostname,&len);

  // Create and commit the particle datatype
  particle_type = SphParticle<ndim>::CreateMpiDataType();
  MPI_Type_commit(&particle_type);

  // Create and commit the particle int datatype
  partint_type = SphIntParticle<ndim>::CreateMpiDataType();
  MPI_Type_commit(&partint_type);

  // Create diagnostics data structure in database
  diagnostics_type = Diagnostics<ndim>::CreateMpiDataType();
  MPI_Type_commit(&diagnostics_type);

  // Create and commit the box datatype
  box_type = CreateBoxType(dummy);
  MPI_Type_commit(&box_type);

  // Allocate buffer to send and receive boxes
  boxes_buffer.resize(Nmpi);

  // Allocate the buffers needed to send and receive particles
  particles_to_export_per_node.resize(Nmpi);
  num_particles_export_per_node.resize(Nmpi);
  particles_to_export.resize(Nmpi);
  displacements_send.resize(Nmpi);
  num_particles_to_be_received.resize(Nmpi);
  receive_displs.resize(Nmpi);

  CreateLeagueCalendar();

#ifdef VERIFY_ALL
  if (this->rank == 0)
    printf("MPI working.  Nmpi : %d   rank : %d   hostname : %s\n",
           Nmpi,rank,hostname);
  else
    printf("%d is running too!!\n",this->rank);

  if (Nmpi > 1) {
    if (rank ==0) {
      SphParticle<ndim> particle;
      particle.gradrho[ndim-1]=-1;
      MPI_Send(&particle,1,particle_type,1,0,MPI_COMM_WORLD);
    }
    else if (rank ==1) {
      SphParticle<ndim> particle;
      MPI_Status status;
      MPI_Recv(&particle,1,particle_type,0,0,MPI_COMM_WORLD,&status);
      if (particle.gradrho[ndim-1]!=-1)
        cerr << "Error in transmitting particles: the last field has not been received correctly!" << endl;
    }
  }
#endif

}



//=============================================================================
//  MpiControl::~MpiControl()
/// MPI node class destructor.
//=============================================================================
template <int ndim>
MpiControl<ndim>::~MpiControl()
{
  MPI_Type_free(&particle_type);
  MPI_Type_free(&partint_type);
  MPI_Type_free(&box_type);
  MPI_Type_free(&diagnostics_type);
}



//=============================================================================
//  MpiControl::AllocateMemory
/// Allocate all memory for MPI control class.
//=============================================================================
template <int ndim>
void MpiControl<ndim>::AllocateMemory(int _Ntot)
{
  mpinode = new MpiNode<ndim>[Nmpi];
  for (int inode=0; inode<Nmpi; inode++) {
	mpinode[inode].Ntotmax = (2*_Ntot)/Nmpi;
	mpinode[inode].ids = new int[mpinode[inode].Ntotmax];
    mpinode[inode].worksent = new FLOAT[Nmpi];
    mpinode[inode].workreceived = new FLOAT[Nmpi];
  }

  return;
}



//=============================================================================
//  MpiControl::DeallocateMemory
/// Deallocate all MPI control class memory.
//=============================================================================
template <int ndim>
void MpiControl<ndim>::DeallocateMemory(void)
{
  for (int inode=0; inode<Nmpi; inode++) {
    delete[] mpinode[inode].worksent;
    delete[] mpinode[inode].workreceived;
  }
  delete[] mpinode;

  return;
}



//=============================================================================
//  MpiControl::CreateLeagueCalendar
/// Create the calendar for the League. In this analogy with soccer,
/// each communication between two nodes is like a football match.
/// Since everybody neads to speak with everybody, this is effectively
/// like a league. We use the scheduling/Berger algorithm
/// (http://en.wikipedia.org/wiki/Round-robin_tournament#Scheduling_algorithm,
/// http://it.wikipedia.org/wiki/Algoritmo_di_Berger) to organize the event.
//=============================================================================
template <int ndim>
void MpiControl<ndim>::CreateLeagueCalendar(void)
{

  //First check that number of processes is even
  if (! Nmpi%2) {
    std::string error = "The number of MPI processes must be even!";
    ExceptionHandler::getIstance().raise(error);
  }

  int Nturns = Nmpi-1;

  //Allocate memory for the calendar
  my_matches.resize(Nturns);


  if (rank==0) {

    //Create a vector containing the full calendar
    //First index is process
    std::vector<std::vector<int> > calendar(Nmpi);
    //And second is turn
    for (int iteam=0; iteam< calendar.size(); iteam++) {
      std::vector<int>& calendar_team = calendar[iteam];
      calendar_team.resize(Nturns);
    }

    // Create pairs table
    std::vector<std::vector<int> > pairs (Nturns);
    for (int iturn=0; iturn< pairs.size(); iturn++) {
      std::vector<int>& pairs_turn = pairs[iturn];
      pairs_turn.resize(Nmpi);
      // Fill in the pairs table
      pairs_turn[0]=Nturns;
      for (int i=1; i<pairs_turn.size(); i++) {
        pairs_turn[i] = (i+iturn) % (Nmpi-1);
      }
      // Can now fill in the calendar
      for (int istep =0; istep<Nmpi/2; istep++) {
        int first_team = pairs_turn[istep];
        int size = pairs_turn.size()-1;
        int second_team = pairs_turn[size-istep];
        calendar[first_team][iturn]=second_team;
        calendar[second_team][iturn]=first_team;
      }
    }


#if defined VERIFY_ALL
    //Validate the calendar
    std::vector<bool> other_teams(Nmpi-1);

    for (int iteam=0; iteam<calendar.size();iteam++) {

      std::fill(other_teams.begin(), other_teams.end(), false);

      for (int iturn=0; iturn<calendar[iteam].size(); iturn++) {
        int opponent = calendar[iteam][iturn];

        //1st check: verify the matches are correctly reported in both locations
        if (calendar[opponent][iturn] != iteam) {
          string msg = "Error 1 in validating the calendar!";
          ExceptionHandler::getIstance().raise(msg);
        }

        //2nd check: verify each team is played against only once
        int index_other_teams = opponent>=iteam ? opponent-1 : opponent;

        if (other_teams[index_other_teams]) {
          string msg = "Error 2 in validating the calendar!";
          ExceptionHandler::getIstance().raise(msg);
        }
        other_teams[index_other_teams] = true;
      }

      for (int jteam; jteam<other_teams.size(); jteam++) {
        if (!other_teams[jteam]) {
          string msg = "Error 3 in validating the calendar!";
          ExceptionHandler::getIstance().raise(msg);
        }
      }
    }

    cout << "Calendar validated!" << endl;

#endif


    //Copy our calendar to the vector
    for (int iturn=0; iturn<Nturns; iturn++) {
      my_matches[iturn] = calendar[0][iturn];
    }


    //Now transmit the calendar to the other nodes
    for (int inode=1; inode < Nmpi; inode++) {
      MPI_Send(&calendar[inode][0], Nturns, MPI_INT, inode, tag_league, MPI_COMM_WORLD);
    }

  }

  else {
    MPI_Status status;
    MPI_Recv(&my_matches[0], Nturns, MPI_INT, 0, tag_league, MPI_COMM_WORLD, &status);

  }


  return;
}



//=============================================================================
//  MpiControl::CreateInitialDomainDecomposition
/// Creates a binary tree containing all particles in order to determine how 
/// to distribute the particles across all MPI nodes with an equal amount of 
/// CPU work per MPI node.  If creating the initial partition (i.e. before 
/// we have calculated the timestep), we give the particles equal weighting 
/// and therefore each node will have equal numbers of particles.  For later 
/// steps (i.e. when we know the timesteps and work information), we split 
/// the domains to give each MPI node equal amounts of work.  This routine 
/// should only be called for the root process.
//=============================================================================
template <int ndim>
void MpiControl<ndim>::CreateInitialDomainDecomposition
(Sph<ndim> *sph,                   ///< Pointer to main SPH object
 Nbody<ndim> *nbody,               ///< Pointer to main N-body object
 Parameters *simparams,            ///< Simulation parameters
 DomainBox<ndim> simbox)           ///< Simulation domain box
{
  int i;                           // Particle counter
  int inode;                       // Node counter
  int k;                           // Dimension counter
  int okflag;                      // ..
  FLOAT boxbuffer[2*ndim*Nmpi];    // Bounding box buffer
  SphParticle<ndim> *partbuffer;   // ..


  // For main process, create load balancing tree, transmit information to all
  // other nodes including particle data
  //---------------------------------------------------------------------------
  if (rank == 0) {

    debug2("[MpiControl::CreateInitialDomainDecomposition]");

    // Create MPI binary tree for organising domain decomposition
    mpitree = new MpiTree<ndim>(Nmpi);

    // Create binary tree from all SPH particles
    // Set number of tree members to total number of SPH particles (inc. ghosts)
    mpitree->Nsph = sph->Nsph;
    mpitree->Ntot = sph->Nsph;
    mpitree->Ntotmax = max(mpitree->Ntot,mpitree->Ntotmax);
    mpitree->gtot = 0;

    // Create all other MPI node objects
    AllocateMemory(mpitree->Ntotmax);


    for (i=0; i<sph->Nsph; i++)
      for (k=0; k<ndim; k++) sph->rsph[ndim*i + k] = sph->sphdata[i].r[k];

    // For periodic simulations, set bounding box of root node to be the 
    // periodic box size.  Otherwise, set to be the particle bounding box.
    if (simbox.x_boundary_lhs == "open") mpibox.boxmin[0] = -big_number;
    else mpibox.boxmin[0] = simbox.boxmin[0];
    if (simbox.x_boundary_rhs == "open") mpibox.boxmax[0] = big_number;
    else mpibox.boxmax[0] = simbox.boxmax[0];
    if (ndim > 1) {
      if (simbox.y_boundary_lhs == "open") mpibox.boxmin[1] = -big_number;
      else mpibox.boxmin[1] = simbox.boxmin[1];
      if (simbox.y_boundary_rhs == "open") mpibox.boxmax[1] = big_number;
      else mpibox.boxmax[1] = simbox.boxmax[1];
    }
    if (ndim == 3) {
      if (simbox.z_boundary_lhs == "open") mpibox.boxmin[2] = -big_number;
      else mpibox.boxmin[2] = simbox.boxmin[2];
      if (simbox.z_boundary_rhs == "open") mpibox.boxmax[2] = big_number;
      else mpibox.boxmax[2] = simbox.boxmax[2];
    }
    mpitree->box = &mpibox;


    // Compute the size of all tree-related arrays now we know number of points
    mpitree->ComputeTreeSize();

    // Allocate (or reallocate if needed) all tree memory
    mpitree->AllocateTreeMemory();

    // Create tree data structure including linked lists and cell pointers
    mpitree->CreateTreeStructure(mpinode);

    // Find ordered list of ptcl positions ready for adding particles to tree
    mpitree->OrderParticlesByCartCoord(sph->sphdata);

    // Now add particles to tree depending on Cartesian coordinates
    mpitree->LoadParticlesToTree(sph->rsph);

    // Create bounding boxes containing particles in each sub-tree
    for (inode=0; inode<Nmpi; inode++) {
      //for (k=0; k<ndim; k++) mpinode[inode].domain.boxmin[k] =
      //  mpitree->subtrees[inode]->box.boxmin[k];
      //for (k=0; k<ndim; k++) mpinode[inode].domain.boxmax[k] =
      //  mpitree->subtrees[inode]->box.boxmax[k];
      cout << "MPIDOMAIN : " << mpinode[inode].domain.boxmin[0] << "     " << mpinode[inode].domain.boxmax[0] << endl;
    }


    // Pack all bounding box data into single array
    for (inode=0; inode<Nmpi; inode++) {
      for (k=0; k<ndim; k++) 
	boxbuffer[2*ndim*inode + k] = mpinode[inode].domain.boxmin[k];
      for (k=0; k<ndim; k++) 
	boxbuffer[2*ndim*inode + ndim + k] = mpinode[inode].domain.boxmax[k];
    }

    // Now broadcast all bounding boxes to other processes
    MPI_Bcast(boxbuffer,2*ndim*Nmpi,MPI_DOUBLE,0,MPI_COMM_WORLD);

    // Send particles to all other domains
    for (inode=1; inode<Nmpi; inode++) {
      SendParticles(inode, mpinode[inode].Ntot,
                    mpinode[inode].ids, sph->sphdata);
      cout << "Sent " << mpinode[inode].Nsph
           << " particles to node " << inode << endl;
    }

    cout << "Sent all particles to other processes" << endl;

    // Delete all other particles from local domain
    sph->Nsph = mpinode[0].Nsph;
    partbuffer = new SphParticle<ndim>[sph->Nsph];
    for (i=0; i<sph->Nsph; i++) partbuffer[i] = sph->sphdata[mpinode[0].ids[i]];
    for (i=0; i<sph->Nsph; i++) sph->sphdata[i] = partbuffer[i];
    delete[] partbuffer;
    cout << "Deleted all other particles from root node" << endl;

  }

  // For other nodes, receive all bounding box and particle data once
  // transmitted by main process.
  //---------------------------------------------------------------------------
  else {

    // Create MPI node objects
    AllocateMemory(sph->Nsph);

    // Receive bounding box data for domain and unpack data
    MPI_Bcast(boxbuffer,2*ndim*Nmpi,MPI_DOUBLE,0,MPI_COMM_WORLD);

    // Unpack all bounding box data
    for (inode=0; inode<Nmpi; inode++) {
      for (k=0; k<ndim; k++)
        mpinode[inode].domain.boxmin[k] = boxbuffer[2*ndim*inode + k];
      for (k=0; k<ndim; k++)
        mpinode[inode].domain.boxmax[k] = boxbuffer[2*ndim*inode + ndim + k];
      if (rank == 1) {
      cout << "Node " << i << endl;
      cout << "xbox : " << mpinode[inode].domain.boxmin[0]
           << "    " << mpinode[inode].domain.boxmax[0] << endl;
      cout << "ybox : " << mpinode[inode].domain.boxmin[1] << "    "
           << mpinode[inode].domain.boxmax[1] << endl;
      cout << "zbox : " << mpinode[inode].domain.boxmin[2] << "    "
           << mpinode[inode].domain.boxmax[2] << endl;
      }
    }
    // Now, receive particles form main process and copy to local main array
    ReceiveParticles(0, (sph->Nsph), &partbuffer);

    sph->AllocateMemory(sph->Nsph);
    mpinode[rank].Nsph = sph->Nsph;

    cout << "Received particles on node " << rank << "   Nsph : " << sph->Nsph << endl;

    for (i=0; i<sph->Nsph; i++) sph->sphdata[i] = partbuffer[i];
    delete[] partbuffer;
    cout << "Deallocated partbuffer" << endl;

  }
  //---------------------------------------------------------------------------

  return;
}



//=============================================================================
//  MpiControl::UpdateAllBoundingBoxes
/// Update local copy of all bounding boxes from all other MPI domains.
//=============================================================================
template <int ndim>
void MpiControl<ndim>::UpdateAllBoundingBoxes
(int Npart,                         ///< No. of SPH particles
 SphParticle<ndim> *sphdata,        ///< Pointer to SPH data
 SphKernel<ndim> *kernptr)          ///< Pointer to kernel object
{
  int inode;                        // MPI node counter

  if (rank == 0) debug2("[MpiControl::UpdateAllBoundingBoxes]");

  // Update local bounding boxes
  mpinode[rank].UpdateBoundingBoxData(Npart,sphdata,kernptr);

  // Do an all_gather to receive the new array
  MPI_Allgather(&mpinode[rank].hbox,1,box_type,&boxes_buffer[0],
                1,box_type,MPI_COMM_WORLD);

  // Save the information inside the nodes
  for (inode=0; inode<Nmpi; inode++) {
    mpinode[inode].hbox = boxes_buffer[inode];
  }

  MPI_Barrier(MPI_COMM_WORLD);

  // Do an all_gather to receive the new array
  MPI_Allgather(&mpinode[rank].rbox,1,box_type,&boxes_buffer[0],
                1,box_type,MPI_COMM_WORLD);

  // Save the information inside the nodes
  for (inode=0; inode<Nmpi; inode++) {
    mpinode[inode].rbox = boxes_buffer[inode];
  }

  return;
}



//=============================================================================
//  MpiControl::LoadBalancing
/// If we are on a load balancing step, then determine which level of 
/// the binary partition we are adjusting for load balancing.  Next, adjust 
/// the domain boundaries at that level (and for all child domains).
/// Then broadcast the new domain boundaries to all other nodes to determine 
/// which particles should be transfered to new nodes.
//=============================================================================
template <int ndim>
void MpiControl<ndim>::LoadBalancing
(Sph<ndim> *sph,                    ///< Pointer to main SPH object
 Nbody<ndim> *nbody)                ///< Pointer to main N-body object
{
  int c;                            // MPI tree cell counter
  int c2;                           // ..
  int i;                            // Particle counter
  int inode;                        // MPI node counter
  int k;                            // Dimension counter
  int kk;                           // ..
  int okflag;                       // Successful communication flag
  FLOAT rnew;                       // New boundary position for load balancing
  FLOAT boxbuffer[2*ndim*Nmpi];     // Bounding box buffer
  FLOAT workbuffer[1+ndim+Nmpi];    // Node work information buffer
  MPI_Status status;                // MPI status flag

  // If running on only one MPI node, return immediately
  if (Nmpi == 1) return;


  // Compute work that will be transmitted to all other domains if using
  // current domain boxes and particle positions
  // (DAVID : Maybe in the future once basic implementation works)

  // Compute total work contained in domain at present and the weighted 
  // centre of work position.
  mpinode[rank].worktot = 0.0;
  for (inode=0; inode<Nmpi; inode++) mpinode[rank].worksent[inode] = 0.0;
  for (inode=0; inode<Nmpi; inode++) mpinode[rank].workreceived[inode] = 0.0;

  for (k=0; k<ndim; k++) mpinode[rank].rwork[k] = 0.0;
  for (i=0; i<sph->Nsph; i++) {
    mpinode[rank].worktot += 1.0/(FLOAT) sph->sphintdata[i].nstep;
    for (k=0; k<ndim; k++) 
      mpinode[rank].rwork[k] += sph->sphdata[i].r[k]/(FLOAT) sph->sphintdata[i].nstep;
  }
  for (k=0; k<ndim; k++) mpinode[rank].rwork[k] /= mpinode[rank].worktot;

  for (inode=0; inode<Nmpi; inode++) cout << "CHECKING DOMAIN : " << rank << "   " << inode << "   " << mpinode[inode].domain.boxmin[0] << "    " << mpinode[inode].domain.boxmax[0] << endl;

  // Now find total work transfered to all other nodes if node
  // boundaries remained unchanged.
  for (i=0; i<sph->Nsph; i++) {
    SphParticle<ndim>& part = sph->sphdata[i];

    // Loop over potential domains and see if we need to transfer this particle to them
    for (inode=0; inode<Nmpi; inode++) {
      //if (rank == 1) cout << "R : " << ParticleInBox(part,mpinode[inode].domain) << "    " << inode << "     " << rank << "    " << part.r[0] << "    " << mpinode[inode].domain.boxmin[0] << "    " << mpinode[inode].domain.boxmax[0] << endl;
      if (ParticleInBox(part,mpinode[inode].domain)) {
        if (inode == rank) break;
        mpinode[rank].worksent[inode] += 1.0/(FLOAT) sph->sphintdata[i].nstep;
        if (rank == 1) cout << "OVERLAP?? : " << rank << "    " << mpinode[rank].worksent[inode] << endl;
        // The particle can belong only to one domain, so we can break from this loop
        break;
      }
    }
  }
  for (inode=0; inode<Nmpi; inode++) mpinode[inode].workreceived[rank] = mpinode[rank].worksent[k];


  cout << "Work sent1 by " << rank << " : ";
  for (k=0; k<Nmpi; k++) cout <<  mpinode[rank].worksent[k] << "    ";
  cout << endl;


  cout << "worktot[" << rank << "] : " << mpinode[rank].worktot << "     Nsph : " << sph->Nsph << endl;
  cout << "rwork : " << mpinode[rank].rwork[0] << endl;


  // For root process, receive work information from all other CPU nodes, 
  // adjust domain boundaries to balance out work more evenly and then 
  // broadcast new domain boxes to all other nodes.
  //---------------------------------------------------------------------------
  if (rank == 0) {

    debug2("[MpiControl::LoadBalancing]");


    // Receive all important load balancing information from other nodes
    for (inode=1; inode<Nmpi; inode++) {
      cout << "Root waiting for " << inode << endl;
      okflag = MPI_Recv(&workbuffer,Nmpi+ndim+1,MPI_DOUBLE,
                        inode,0,MPI_COMM_WORLD,&status);
      mpinode[inode].worktot = workbuffer[0];
      for (k=0; k<ndim; k++) mpinode[inode].rwork[k] = workbuffer[k+1];
      for (k=0; k<Nmpi; k++) mpinode[inode].worksent[k] = workbuffer[ndim+k+1];
      for (k=0; k<Nmpi; k++) mpinode[k].workreceived[inode] = workbuffer[ndim+k+1];
      cout << "Work from rank " << inode << " : " << workbuffer[0] << endl;
    }

	MPI_Barrier(MPI_COMM_WORLD);

	for (inode=0; inode<Nmpi; inode++) {
      cout << "Work sent by " << inode << " : ";
      for (k=0; k<Nmpi; k++) cout <<  mpinode[inode].worksent[k] << "    ";
      cout << endl;
	}

    cout << "Done receiving boxes " << endl;

    // Walk upwards through MPI tree to propagate work information from 
    // leaf cells to parent cells.
    //-------------------------------------------------------------------------
    for (c=mpitree->Ncell-1; c>=0; c--) {
      
      // For leaf cells, simply set total work and 'centre of work' position
      if (mpitree->tree[c].c2 == 0) {
        inode = mpitree->tree[c].c2g;
        mpitree->tree[c].worktot = mpinode[inode].worktot;
        for (k=0; k<ndim; k++) mpitree->tree[c].rwork[k] = mpinode[inode].rwork[k];
      }
      // For non-leaf cells, sum up contributions from both child cells
      else {
        c2 = mpitree->tree[c].c2;
        mpitree->tree[c].worktot = mpitree->tree[c+1].worktot + mpitree->tree[c2].worktot;
        for (k=0; k<ndim; k++) mpitree->tree[c].rwork[k] =
          (mpitree->tree[c].worktot*mpitree->tree[c].rwork[k] +
           mpitree->tree[c2].worktot*mpitree->tree[c2].rwork[k]) / mpitree->tree[c].worktot;
      }

      cout << "Tree work : " << c << "    " << mpitree->tree[c].worktot << endl;

    }
    //-------------------------------------------------------------------------


    // Work out tree level at which we are altering the load balancing.  
    // Stars at lowest level and then works back up wrapping around to the 
    // bottom again once reaching the root node.
    balance_level--;
    if (balance_level < 0) balance_level = mpitree->ltot - 1;
    k = mpitree->klevel[balance_level];

    cout << "Balancing on level : " << balance_level << "     " << mpitree->ltot << "    " << k << endl;


    // Now walk downwards through MPI tree, adjusting bounding box sizes on 
    // load balancing level and propagating new box sizes to lower levels.
    //-------------------------------------------------------------------------
    for (c=0; c<mpitree->Ncell; c++) {
      c2 = mpitree->tree[c].c2;

      cout << "Checking cell " << c << "   " << c2 << "     " << mpitree->tree[c].clevel << endl;

      // If on load-balancing level, then estimate new division of domains 
      // based on work in each domain and position of 'centre of work'.
      // If work is already balanced, then domain boundaries remain unchanged.
      if (mpitree->tree[c].clevel == balance_level && c2 != 0) {
        //if (mpitree->tree[c+1].worktot > mpitree->tree[c2].worktot)
        //  rnew = mpitree->tree[c+1].rwork[k] +
        //    2.0*(mpitree->tree[c+1].bbmax[k] - mpitree->tree[c+1].rwork[k])*
        //    mpitree->tree[c+1].worktot/mpitree->tree[c].worktot;
        //else
	    //  rnew = mpitree->tree[c2].rwork[k] +
	    //    2.0*(mpitree->tree[c2].bbmin[k] - mpitree->tree[c2].rwork[k])*
	    //    mpitree->tree[c2].worktot/mpitree->tree[c].worktot;
    	int i1 = mpitree->tree[c+1].c2g;
    	int i2 = mpitree->tree[c2].c2g;
    	FLOAT dwdx1 = 0.5*mpitree->tree[c+1].worktot/
    			(mpitree->tree[c+1].bbmax[k] - mpitree->tree[c+1].rwork[k]);
    	FLOAT dwdx2 = 0.5*mpitree->tree[c2].worktot/
    			(mpitree->tree[c2].rwork[k] - mpitree->tree[c2].bbmin[k]);
    	FLOAT dxnew = (mpinode[i1].worksent[i2] + mpinode[i2].worksent[i1])/
    			(dwdx1 + dwdx2);
    	rnew = mpitree->tree[c+1].bbmax[k] + dxnew;
    	cout << "dwdx : " << dwdx1 << "    " << dwdx2 << "      dxnew : " << dxnew << endl;
    	cout << "worksent : " << mpinode[i1].worksent[i2] << "    " << mpinode[i2].worksent[i1] << endl;
    	cout << "rold : " << mpitree->tree[c+1].bbmax[k] << "     rnew : " << rnew << endl;

    	mpitree->tree[c+1].bbmax[k] = rnew;
        mpitree->tree[c2].bbmin[k] = rnew;
        cout << "work : " << mpitree->tree[c+1].worktot << "    " << mpitree->tree[c2].worktot << endl;
        cout << "Child 1 domain : " << mpitree->tree[c+1].bbmin[k] << "     " << mpitree->tree[c+1].bbmax[k] << endl;
        cout << "Child 1 rbox   : " << mpinode[i1].rbox.boxmin[k] << "     " << mpinode[i1].rbox.boxmax[k] << endl;
        cout << "Child 2 domain : " << mpitree->tree[c2].bbmin[k] << "     " << mpitree->tree[c2].bbmax[k] << endl;
        cout << "Child 2 rbox   : " << mpinode[i2].rbox.boxmin[k] << "     " << mpinode[i2].rbox.boxmax[k] << endl;
      }

      // For leaf cells, set new MPI node box sizes 
      else if (c2 == 0) {
        inode = mpitree->tree[c].c2g;
        for (kk=0; kk<ndim; kk++)
          mpinode[inode].domain.boxmin[kk] = mpitree->tree[c].bbmin[kk];
        for (kk=0; kk<ndim; kk++)
          mpinode[inode].domain.boxmax[kk] = mpitree->tree[c].bbmax[kk];
      }

      // For other cells, propagate new bounding box information downwards
      else {
        mpitree->tree[c+1].bbmin[k] = mpitree->tree[c].bbmin[k];
        mpitree->tree[c2].bbmax[k] = mpitree->tree[c].bbmax[k];
      }

    }
    //-------------------------------------------------------------------------


    // Transmit new bounding box sizes to all other nodes
    for (inode=0; inode<Nmpi; inode++) {
      for (k=0; k<ndim; k++)
        boxbuffer[2*ndim*inode + k] = mpinode[inode].domain.boxmin[k];
      for (k=0; k<ndim; k++)
        boxbuffer[2*ndim*inode + ndim + k] = mpinode[inode].domain.boxmax[k];

      cout << "New box for node " << inode << "    : " << mpinode[inode].domain.boxmin[0] << "     " << mpinode[inode].domain.boxmax[0] << endl;

    }

    // Now broadcast all bounding boxes to other processes
    MPI_Bcast(boxbuffer,2*ndim*Nmpi,MPI_DOUBLE,0,MPI_COMM_WORLD);
    MPI_Barrier(MPI_COMM_WORLD);

  }
  //---------------------------------------------------------------------------
  else {


	cout << "Node : " << rank << " sending information" << endl;

    // Transmit load balancing information to main root node
    workbuffer[0] = mpinode[rank].worktot;
    for (k=0; k<ndim; k++) workbuffer[k+1] = mpinode[rank].rwork[k];
    for (k=0; k<Nmpi; k++) workbuffer[ndim+k+1] = mpinode[rank].worksent[k];
    okflag = MPI_Send(&workbuffer,ndim+1,MPI_DOUBLE,0,0,MPI_COMM_WORLD);
	MPI_Barrier(MPI_COMM_WORLD);

	cout << "Node " << rank << " waiting to receive all boxes" << endl;

    // Receive bounding box data for domain and unpack data
    MPI_Bcast(boxbuffer,2*ndim*Nmpi,MPI_DOUBLE,0,MPI_COMM_WORLD);

    // Unpack all bounding box data
    for (inode=0; inode<Nmpi; inode++) {
      for (k=0; k<ndim; k++) 
        mpinode[inode].domain.boxmin[k] = boxbuffer[2*ndim*inode + k];
      for (k=0; k<ndim; k++) 
        mpinode[inode].domain.boxmax[k] = boxbuffer[2*ndim*inode + ndim + k];
    }
    MPI_Barrier(MPI_COMM_WORLD);


  }
  //---------------------------------------------------------------------------



  // Prepare lists of particles that now occupy other processor domains that 
  // need to be transfered

  // First construct the list of nodes that we might be sending particles to
  std::vector<int> potential_nodes;
  potential_nodes.reserve(Nmpi);
  for (int inode=0; inode<Nmpi; inode++) {
    if (inode == rank) continue;
    if (BoxesOverlap(mpinode[inode].domain,mpinode[rank].rbox)) {
      potential_nodes.push_back(inode);
    }
  }



  // Now find the particles that need to be transferred - delegate to NeighbourSearch
  std::vector<std::vector<int> > particles_to_transfer (Nmpi);
  std::vector<int> all_particles_to_export;
  BruteForceSearch<ndim> bruteforce;
  bruteforce.FindParticlesToTransfer(sph, particles_to_transfer, all_particles_to_export, potential_nodes, mpinode);

  // Send and receive particles from/to all other nodes
  std::vector<SphParticle<ndim> > sendbuffer, recvbuffer;
  std::vector<SphIntParticle<ndim> > sendbufferint, recvbufferint;
  for (int iturn = 0; iturn<my_matches.size(); iturn++) {
    int inode = my_matches[iturn];

    int N_to_transfer = particles_to_transfer[inode].size();
    cout << "Transfer!!  Rank : " << rank << "    N_to_transfer : " << N_to_transfer << "    dest : " << inode << endl;
    sendbuffer.clear(); sendbuffer.resize(N_to_transfer);
    sendbufferint.clear(); sendbufferint.resize(N_to_transfer);
    recvbuffer.clear();
    recvbufferint.clear();

    // Copy particles into send buffer
    for (int ipart=0; ipart < N_to_transfer; ipart++) {
      int index = particles_to_transfer[inode][ipart];
      sendbuffer[ipart] = sph->sphdata[index];
      sendbufferint[ipart] = sph->sphintdata[index];
    }

    // Do the actual send and receive

    //Decide if we have to send or receive first
    bool send_turn;
    if (rank < inode) {
      send_turn=true;
    }
    else {
      send_turn=false;
    }

    //Do the actual communication, sending and receiving in the right order
    for (int i=0; i < 2; i++) {
      if (send_turn) {
        cout << "Sending " << N_to_transfer << " from " << rank << " to " << inode << endl;
        MPI_Send(&sendbuffer[0], N_to_transfer, particle_type, inode, tag_bal, MPI_COMM_WORLD);
        MPI_Send(&sendbufferint[0], N_to_transfer, partint_type, inode, tag_bal, MPI_COMM_WORLD);
        send_turn = false;
        cout << "Sent " << N_to_transfer << " from " << rank << " to " << inode << endl;
      }
      else {
        int N_to_receive;
        MPI_Status status;
        MPI_Probe(inode, tag_bal, MPI_COMM_WORLD, &status);
        MPI_Get_count(&status, particle_type, &N_to_receive);
        recvbuffer.resize(N_to_receive); recvbufferint.resize(N_to_receive);
        cout << "Rank " << rank << " receiving " << N_to_receive << " from " << inode << endl;
        if (sph->Nsph+N_to_receive > sph->Nsphmax) {
          cout << "Memory problem : " << rank << " " << sph->Nsph << " " << N_to_receive << " " << sph->Nsphmax <<endl;
          string message = "Not enough memory for transfering particles";
          ExceptionHandler::getIstance().raise(message);
        }
        MPI_Recv(&recvbuffer[0], N_to_receive, particle_type, inode, tag_bal, MPI_COMM_WORLD, &status);
        MPI_Recv(&recvbufferint[0], N_to_receive, partint_type, inode, tag_bal, MPI_COMM_WORLD, &status);
        send_turn = true;
        cout << "Rank " << rank << " received " << N_to_receive << " from " << inode << endl;
      }
    }

    // Copy particles from receive buffer to main arrays
    int running_counter = sph->Nsph;
    // TODO: check we have enough memory
    for (int i=0; i< recvbuffer.size(); i++) {
      sph->sphdata[running_counter] = recvbuffer[i];
      sph->sphintdata[running_counter] = recvbufferint[i];
      sph->sphintdata[running_counter].part = &sph->sphdata[running_counter];
      running_counter++;
    }
    sph->Nsph = running_counter;

  }


  // Remove transferred particles
  sph->DeleteParticles(all_particles_to_export.size(), &all_particles_to_export[0]);




  return;

}



//=============================================================================
//  MpiControl::SendReceiveGhosts
/// Compute particles to send to other nodes and receive needed particles 
/// from other nodes.  Returns the number of ghosts received. The array with 
/// the received particles is stored inside the pointer returned. The caller 
/// must NOT delete this array when is finished with it, as the memory is 
/// internally managed by this class.
//=============================================================================
template <int ndim>
int MpiControl<ndim>::SendReceiveGhosts
(SphParticle<ndim>** array,        ///< Main SPH particle array
 Sph<ndim>* sph)                   ///< Main SPH object pointer
{
  int i;                           // Particle counter
  int index;                       // ..
  int running_counter;             // ..
  int inode;
  std::vector<int > overlapping_nodes;

  if (rank == 0) debug2("[MpiControl::SendReceiveGhosts]");

  //Reserve space for all nodes
  overlapping_nodes.reserve(Nmpi);

  //Loop over domains and find the ones that could overlap us
  for (inode=0; inode<Nmpi; inode++) {
    if (inode == rank) continue;
    if (BoxesOverlap(mpinode[inode].hbox,mpinode[rank].hbox)) {
      overlapping_nodes.push_back(inode);
    }
  }

  //Clear the buffer holding the indexes of the particles we need to export
  for (inode=0; inode<particles_to_export_per_node.size(); inode++) {
    particles_to_export_per_node[inode].clear();
  }

  //Ask the neighbour search class to compute the list of particles to export
  //For now, hard-coded the BruteForce class
  BruteForceSearch<ndim> bruteforce;
  bruteforce.FindGhostParticlesToExport(sph,particles_to_export_per_node,overlapping_nodes,mpinode);

  //Prepare arrays with number of particles to export per node and displacements
  std::fill(num_particles_export_per_node.begin(),num_particles_export_per_node.end(),0);

  running_counter = 0;
  for (int inode=0; inode<Nmpi; inode++) {
    int num_particles = particles_to_export_per_node[inode].size();
    num_particles_export_per_node[inode] = num_particles;
    displacements_send[inode]=running_counter;
    running_counter += num_particles;
  }

  //Compute total number of particles to export
  int tot_particles_to_export = std::accumulate(num_particles_export_per_node.begin(),num_particles_export_per_node.end(),0);

  //Comunicate with all the processors the number of particles that everyone is exporting
  std::vector<int> ones (Nmpi,1);
  std::vector<int> displs(Nmpi);
  for (inode=0; inode<displs.size(); inode++) {
    displs[inode] = inode;
  }
  MPI_Alltoallv(&num_particles_export_per_node[0], &ones[0], &displs[0], MPI_INT, &num_particles_to_be_received[0], &ones[0], &displs[0], MPI_INT, MPI_COMM_WORLD);

  //Compute the total number of particles that will be received
  tot_particles_to_receive = std::accumulate(num_particles_to_be_received.begin(),num_particles_to_be_received.end(),0);

  //Allocate receive buffer
  particles_receive.clear();
  particles_receive.resize(tot_particles_to_receive);

  //Create vector containing all particles to export
  particles_to_export.resize(tot_particles_to_export);

  index = 0;
  for (inode=0; inode < Nmpi; inode++) {
    std::vector<SphParticle<ndim>* >& particles_on_this_node = particles_to_export_per_node[inode];
    for (int iparticle=0; iparticle<particles_on_this_node.size(); iparticle++) {
      particles_to_export[index] =  *particles_on_this_node[iparticle];
      index++;
    }
  }

  assert(index==tot_particles_to_export);

  //Compute receive displacements
  running_counter = 0;
  for (inode=0; inode<receive_displs.size(); inode++) {
    receive_displs[inode] = running_counter;
    running_counter += num_particles_to_be_received[inode];
  }

  //Send and receive particles
  MPI_Alltoallv(&particles_to_export[0], &num_particles_export_per_node[0],
                &displacements_send[0], particle_type, &particles_receive[0],
                &num_particles_to_be_received[0], &receive_displs[0],
                particle_type, MPI_COMM_WORLD);

  *array = &particles_receive[0];

  return tot_particles_to_receive;

}



//=============================================================================
//  MpiControl::UpdateGhostParticles
/// Update the ghost particles that were previously found. Return the number 
/// of Mpi Ghosts; modified the passed pointer with the address of the array 
/// of ghosts (note: the caller must NOT call delete on this array, as the 
/// memory is internally managed by the MpiControl class).
//=============================================================================
template <int ndim>
int MpiControl<ndim>::UpdateGhostParticles
(SphParticle<ndim>** array)         ///< Main SPH particle array
{
  int index = 0;                    // ..
  int inode;                        // MPI node counter
  int ipart;                        // Particle counter

  //Update the local buffer of particles to send
  for (inode=0; inode<Nmpi; inode++) {
    std::vector<SphParticle<ndim>* >& particles_on_this_node = particles_to_export_per_node[inode];
    for (ipart=0; ipart<particles_on_this_node.size(); ipart++) {
      particles_to_export[index] = *particles_on_this_node[ipart];
      index++;
    }
  }

  //Send and receive particles
  MPI_Alltoallv(&particles_to_export[0], &num_particles_export_per_node[0],
                &displacements_send[0], particle_type, &particles_receive[0],
                &num_particles_to_be_received[0], &receive_displs[0],
                particle_type, MPI_COMM_WORLD);

  *array = &particles_receive[0];

  return tot_particles_to_receive;
}



//=============================================================================
//  MpiControl::SendParticles
/// Given an array of ids and a node, copy particles inside a buffer and 
/// send them to the given node.
//=============================================================================
template <int ndim>
void MpiControl<ndim>::SendParticles
(int Node, 
 int Nparticles, 
 int* list, 
 SphParticle<ndim>* main_array) 
{
  int i;                            // Particle counter

  //Ensure there is enough memory in the buffer
  sendbuffer.resize(Nparticles);

  //Copy particles from the main arrays to the buffer
  for (i=0; i<Nparticles; i++) {
    sendbuffer[i] = main_array[list[i]];
  }

  MPI_Send(&sendbuffer[0], Nparticles, particle_type, Node, 
          tag_srpart, MPI_COMM_WORLD);

  return;
}



//=============================================================================
//  MpiControl::ReceiveParticles
/// Given a node, receive particles from it. Return the number of particles 
/// received and a pointer to the array containing the particles. The caller 
/// is reponsible to free the array after its usage.
//=============================================================================
template <int ndim>
void MpiControl<ndim>::ReceiveParticles
(int Node, 
 int& Nparticles, 
 SphParticle<ndim>** array) 
{

  const int tag = 1;
  MPI_Status status;

  //"Probe" the message to know how big the message is going to be
  MPI_Probe(Node, tag, MPI_COMM_WORLD, &status);

  //Get the number of elements
  MPI_Get_count(&status, particle_type, &Nparticles);

  //Allocate enough memory to hold the particles
  *array = new SphParticle<ndim> [Nparticles];

  //Now receive the message
  MPI_Recv(*array, Nparticles, particle_type, Node, 
           tag_srpart, MPI_COMM_WORLD, &status);

  return;
}



//=============================================================================
//  MpiControl::CollateDiagnosticsData
/// ..
//=============================================================================
template <int ndim>
void MpiControl<ndim>::CollateDiagnosticsData(Diagnostics<ndim> &diag)
{
  int inode;
  int j;
  int k;
  Diagnostics<ndim> diagaux;
  MPI_Status status;

  //---------------------------------------------------------------------------
  if (rank == 0) {

    // First multiply root node values by mass
    for (k=0; k<ndim; k++) diag.rcom[k] *= diag.mtot;
    for (k=0; k<ndim; k++) diag.vcom[k] *= diag.mtot;

    for (inode=1; inode<Nmpi; inode++) {
      MPI_Recv(&diagaux, 1, diagnostics_type, 
               inode, 0, MPI_COMM_WORLD, &status);
      diag.Nsph += diagaux.Nsph;
      diag.Nstar += diagaux.Nstar;
      diag.Etot += diagaux.Etot;
      diag.utot += diagaux.utot;
      diag.ketot += diagaux.ketot;
      diag.gpetot += diagaux.gpetot;
      diag.mtot += diagaux.mtot;
      for (k=0; k<ndim; k++) diag.mom[k] += diagaux.mom[k];
      for (k=0; k<3; k++) diag.angmom[k] += diagaux.angmom[k];
      for (k=0; k<ndim; k++) diag.force[k] += diagaux.force[k];
      for (k=0; k<ndim; k++) diag.force_hydro[k] += diagaux.force_hydro[k];
      for (k=0; k<ndim; k++) diag.force_grav[k] += diagaux.force_grav[k];
      for (k=0; k<ndim; k++) diag.rcom[k] += diagaux.mtot*diagaux.rcom[k];
      for (k=0; k<ndim; k++) diag.vcom[k] += diagaux.mtot*diagaux.vcom[k];
    }

    // Renormalise centre of mass positions and velocities
    for (k=0; k<ndim; k++) diag.rcom[k] /= diag.mtot;
    for (k=0; k<ndim; k++) diag.vcom[k] /= diag.mtot;

  }
  //---------------------------------------------------------------------------
  else {

    MPI_Send(&diag, 1, diagnostics_type, 0, 0, MPI_COMM_WORLD);

  }
  //---------------------------------------------------------------------------

  return;
}



// Template class instances for each dimensionality value (1, 2 and 3)
template class MpiControl<1>;
template class MpiControl<2>;
template class MpiControl<3>;
