/* ----------------------------------------------------------------------
   LIGGGHTS - LAMMPS Improved for General Granular and Granular Heat
   Transfer Simulations

   LIGGGHTS is part of the CFDEMproject
   www.liggghts.com | www.cfdem.com

   Christoph Kloss, christoph.kloss@cfdem.com
   Copyright 2009-2012 JKU Linz
   Copyright 2012-     DCS Computing GmbH, Linz

   LIGGGHTS is based on LAMMPS
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   This software is distributed under the GNU General Public License.

   See the README file in the top-level directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing authors:
   Christoph Kloss (JKU Linz, DCS Computing GmbH, Linz)
   Richard Berger (JKU Linz)
------------------------------------------------------------------------- */
#ifndef PAIR_GRAN_BASE_H_
#define PAIR_GRAN_BASE_H_

#include "contact_interface.h"
#include "pair_gran.h"
#include "neighbor.h"
#include "neigh_list.h"
#include <malloc.h>

#if defined(_WIN32) || defined(_WIN64)
static inline void * aligned_malloc(size_t alignment, size_t size)
{
  return _aligned_malloc(size, alignment);
}

static inline void aligned_free(void * ptr)
{
  _aligned_free(ptr);
}
#else
static inline void * aligned_malloc(size_t alignment, size_t size)
{
  return memalign(alignment, size);
}

static inline void aligned_free(void * ptr)
{
  free(ptr);
}
#endif

namespace LAMMPS_NS
{
using namespace ContactModels;

template<typename ContactModel>
class PairGranBase : public PairGran {
  CollisionData * aligned_cdata;
  ForceData * aligned_i_forces;
  ForceData * aligned_j_forces;
  PairContactHistorySetup hsetup;
  ContactModel cmodel;

  inline void force_update(double * const f, double * const torque,
      const ForceData & forces) {
    for (int coord = 0; coord < 3; coord++) {
      f[coord] += forces.delta_F[coord];
      torque[coord] += forces.delta_torque[coord];
    }
  }

protected:
  virtual bool forceoff(){ return false; };

public:
  PairGranBase(class LAMMPS * lmp) : PairGran(lmp),
    aligned_cdata((CollisionData*)aligned_malloc(32, sizeof(CollisionData))),
    aligned_i_forces((ForceData*)aligned_malloc(32, sizeof(ForceData))),
    aligned_j_forces((ForceData*)aligned_malloc(32, sizeof(ForceData))),
    hsetup(this), cmodel(lmp, &hsetup) {
  }

  virtual ~PairGranBase() {
    aligned_free(aligned_cdata);
    aligned_free(aligned_i_forces);
    aligned_free(aligned_j_forces);
  }

  virtual void settings(int nargs, char ** args) {
    Settings settings(lmp);
    cmodel.registerSettings(settings);
    settings.parseArguments(nargs, args);
  }

  virtual void init_granular() {
    cmodel.connectToProperties(force->registry);
  }

  virtual void write_restart_settings(FILE * fp)
  {
    int64_t hashcode = ContactModel::STYLE_HASHCODE;
    fwrite(&hashcode, sizeof(int64_t), 1, fp);
  }

  virtual void read_restart_settings(FILE * fp)
  {
    int me = comm->me;
    int64_t hashcode = -1;
    if(me == 0){
      fread(&hashcode, sizeof(int64_t), 1, fp);
      // sanity check
      if(hashcode != ContactModel::STYLE_HASHCODE)
        error->all(FLERR,"wrong pair style loaded!");
    }
  }

  virtual void compute_force(int eflag, int vflag, int addflag)
  {
    if (eflag || vflag)
      ev_setup(eflag, vflag);
    else
      evflag = vflag_fdotr = 0;

    double **x = atom->x;
    double **v = atom->v;
    double **f = atom->f;
    double **omega = atom->omega;
    double **torque = atom->torque;
    double *radius = atom->radius;
    double *rmass = atom->rmass;
    double *mass = atom->mass;
    int *type = atom->type;
    int *mask = atom->mask;
    int nlocal = atom->nlocal;
    const int newton_pair = force->newton_pair;

    int inum = list->inum;
    int * ilist = list->ilist;
    int * numneigh = list->numneigh;

    int ** firstneigh = list->firstneigh;
    int ** firsttouch = listgranhistory ? listgranhistory->firstneigh : NULL;
    double ** firstshear = listgranhistory ? listgranhistory->firstdouble : NULL;

    const int dnum = PairGran::dnum();

    // clear data, just to be safe
    memset(aligned_cdata, 0, sizeof(CollisionData));
    memset(aligned_i_forces, 0, sizeof(ForceData));
    memset(aligned_j_forces, 0, sizeof(ForceData));
    aligned_cdata->area_ratio = 1.0;

    CollisionData cdata  = *aligned_cdata;
    ForceData & i_forces = *aligned_i_forces;
    ForceData & j_forces = *aligned_j_forces;
    cdata.is_wall = false;
    cdata.computeflag = computeflag;
    cdata.shearupdate = shearupdate;

    cmodel.beginPass(cdata, i_forces, j_forces);

    // loop over neighbors of my atoms

    for (int ii = 0; ii < inum; ii++) {
      const int i = ilist[ii];
      const double xtmp = x[i][0];
      const double ytmp = x[i][1];
      const double ztmp = x[i][2];
      const double radi = radius[i];
      int * const touch = firsttouch ? firsttouch[i] : NULL;
      double * const allshear = firstshear ? firstshear[i] : NULL;
      int * const jlist = firstneigh[i];
      const int jnum = numneigh[i];

      cdata.i = i;
      cdata.radi = radi;

      for (int jj = 0; jj < jnum; jj++) {
        const int j = jlist[jj] & NEIGHMASK;

        const double delx = xtmp - x[j][0];
        const double dely = ytmp - x[j][1];
        const double delz = ztmp - x[j][2];
        const double rsq = delx * delx + dely * dely + delz * delz;
        const double radj = radius[j];
        const double radsum = radi + radj;

        cdata.j = j;
        cdata.delta[0] = delx;
        cdata.delta[1] = dely;
        cdata.delta[2] = delz;
        cdata.rsq = rsq;
        cdata.radj = radj;
        cdata.radsum = radsum;
        cdata.touch = touch ? &touch[jj] : NULL;
        cdata.contact_history = allshear ? &allshear[dnum*jj] : NULL;

        if (rsq < radsum * radsum) {
          const double r = sqrt(rsq);
          const double rinv = 1.0 / r;

          // unit normal vector
          const double enx = delx * rinv;
          const double eny = dely * rinv;
          const double enz = delz * rinv;

          // meff = effective mass of pair of particles
          // if I or J part of rigid body, use body mass
          // if I or J is frozen, meff is other particle
          double mi, mj;
          const int itype = type[i];
          const int jtype = type[j];

          if (rmass) {
            mi = rmass[i];
            mj = rmass[j];
          } else {
            mi = mass[itype];
            mj = mass[jtype];
          }
          if (fix_rigid) {
            if (mass_rigid[i] > 0.0) mi = mass_rigid[i];
            if (mass_rigid[j] > 0.0) mj = mass_rigid[j];
          }

          double meff = mi * mj / (mi + mj);
          if (mask[i] & freeze_group_bit)
            meff = mj;
          if (mask[j] & freeze_group_bit)
            meff = mi;

          // copy collision data to struct (compiler can figure out a better way to
          // interleave these stores with the double calculations above.
          cdata.itype = itype;
          cdata.jtype = jtype;
          cdata.r = r;
          cdata.rinv = rinv;
          cdata.meff = meff;
          cdata.mi = mi;
          cdata.mj = mj;
          cdata.en[0]   = enx;
          cdata.en[1]   = eny;
          cdata.en[2]   = enz;
          cdata.v_i     = v[i];
          cdata.v_j     = v[j];
          cdata.omega_i = omega[i];
          cdata.omega_j = omega[j];

          cmodel.collision(cdata, i_forces, j_forces);

          // if there is a collision, there will always be a force
          cdata.has_force_update = true;
        } else {
          // apply force update only if selected contact models have requested it
          cdata.has_force_update = false;
          cmodel.noCollision(cdata, i_forces, j_forces);
        }

        if(cdata.has_force_update) {
          if (computeflag) {
            force_update(f[i], torque[i], i_forces);

            if(newton_pair || j < nlocal) {
              force_update(f[j], torque[j], j_forces);
            }
          }

          if (cpl && addflag)
            cpl_add_pair(cdata, i_forces);

          if (evflag)
            Pair::ev_tally_xyz(i, j, nlocal, newton_pair, 0.0, 0.0,i_forces.delta_F[0],i_forces.delta_F[1],i_forces.delta_F[2],cdata.delta[0],cdata.delta[1],cdata.delta[2]);
        }
      }
    }

    cmodel.endPass(cdata, i_forces, j_forces);
  }
};
}
#endif /* PAIR_GRAN_BASE_H_ */
