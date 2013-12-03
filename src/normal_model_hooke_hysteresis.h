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
#ifdef NORMAL_MODEL
NORMAL_MODEL(HOOKE_HYSTERESIS,hooke/hysteresis,2)
#else
#ifndef NORMAL_MODEL_HOOKE_HYSTERESIS_H_
#define NORMAL_MODEL_HOOKE_HYSTERESIS_H_
#include "contact_models.h"
#include "math.h"
#include "atom.h"
#include "force.h"
#include "update.h"
#include "global_properties.h"

namespace ContactModels
{
  template<typename Style>
  class NormalModel<HOOKE_HYSTERESIS, Style> : protected NormalModel<HOOKE, Style>
  {
  public:
    static const int MASK = CM_REGISTER_SETTINGS | CM_CONNECT_TO_PROPERTIES | CM_COLLISION | CM_NO_COLLISION;

    NormalModel(LAMMPS * lmp, IContactHistorySetup * hsetup) : NormalModel<HOOKE, Style>(lmp, hsetup),
        kn2k2Max(NULL),
        kn2kc(NULL),
        phiF(NULL)
    {
      history_offset = hsetup->add_value("deltaMax", "1");
    }

    inline void registerSettings(Settings & settings){
      NormalModel<HOOKE, Style>::registerSettings(settings);
    }

    inline void connectToProperties(PropertyRegistry & registry) {
      NormalModel<HOOKE, Style>::connectToProperties(registry);

      registry.registerProperty("kn2kcMax", &MODEL_PARAMS::createCoeffMaxElasticStiffness);
      registry.registerProperty("kn2kc", &MODEL_PARAMS::createCoeffAdhesionStiffness);
      registry.registerProperty("phiF", &MODEL_PARAMS::createCoeffPlasticityDepth);

      registry.connect("kn2kcMax", kn2k2Max);
      registry.connect("kn2kc", kn2kc);
      registry.connect("phiF", phiF);
    }

    inline void collision(CollisionData & cdata, ForceData & i_forces, ForceData & j_forces)
    {
      // use these values from HOOKE implementation
      bool & viscous = NormalModel<HOOKE, Style>::viscous;
      double ** & Yeff = NormalModel<HOOKE, Style>::Yeff;
      double & charVel = NormalModel<HOOKE, Style>::charVel;
      bool & tangential_damping = NormalModel<HOOKE, Style>::tangential_damping;
      Force * & force = NormalModel<HOOKE, Style>::force;

      const int itype = cdata.itype;
      const int jtype = cdata.jtype;
      const double deltan = cdata.deltan;
      double ri = cdata.radi;
      double rj = cdata.radj;
      double reff=ri*rj/(ri+rj);
      double meff=cdata.meff;
      double coeffRestLogChosen;

      if (viscous)  {
        double ** & coeffMu = NormalModel<HOOKE, Style>::coeffMu;
        double ** & coeffRestMax = NormalModel<HOOKE, Style>::coeffRestMax;
        double ** & coeffStc = NormalModel<HOOKE, Style>::coeffStc;
        // Stokes Number from MW Schmeeckle (2001)
        const double stokes=cdata.meff*cdata.vn/(6.0*M_PI*coeffMu[itype][jtype]*reff*reff);
        // Empirical from Legendre (2006)
        coeffRestLogChosen=log(coeffRestMax[itype][jtype])+coeffStc[itype][jtype]/stokes;
      } else {
        double ** & coeffRestLog = NormalModel<HOOKE, Style>::coeffRestLog;
        coeffRestLogChosen=coeffRestLog[itype][jtype];
      }

      const double sqrtval = sqrt(reff);
      double kn = 16./15.*sqrtval*(Yeff[itype][jtype])*pow(15.*meff*charVel*charVel/(16.*sqrtval*Yeff[itype][jtype]),0.2);
      double kt = kn;
      const double gamman = sqrt(4.*meff*kn/(1.+(M_PI/coeffRestLogChosen)*(M_PI/coeffRestLogChosen)));
      const double gammat = tangential_damping ? gamman : 0.0;

      // convert Kn and Kt from pressure units to force/distance^2
      kn /= force->nktv2p;
      kt /= force->nktv2p;

      if(cdata.touch) *cdata.touch |= TOUCH_NORMAL_MODEL;
      double * const shear = &cdata.contact_history[history_offset];
      double deltaMax = shear[0]; // the 4th value of the history array is deltaMax
      if (deltan > deltaMax) {
        shear[0] = deltan;
        deltaMax = deltan;
      }

      const double k2Max = kn * kn2k2Max[itype][jtype]; 
      const double kc = kn * kn2kc[itype][jtype]; 

      // k2 dependent on the maximum overlap
      // this accounts for an increasing stiffness with deformation
      const double deltaMaxLim =(k2Max/(k2Max-kn))*phiF[itype][jtype]*2*reff;
      double k2;
      if (deltaMax >= deltaMaxLim) k2 = k2Max;
      else k2 = kn+(k2Max-kn)*deltaMax/deltaMaxLim;

      const double fTmp = k2*(deltan-deltaMax)+kn*deltaMax;//k2*(deltan-delta0);
      double fHys;
      if (fTmp >= kn*deltan) {
        fHys = kn*deltan;
      } else {
        if (fTmp > -kc*deltan) {
          fHys = fTmp;
        } else fHys = -kc*deltan;
      }

      const double Fn_damping = -gamman*cdata.vn;
      const double Fn = fHys + Fn_damping;

      cdata.Fn = Fn;
      cdata.kn = kn;
      cdata.kt = kt;
      cdata.gamman = gamman;
      cdata.gammat = gammat;

      // apply normal force
      if(cdata.is_wall) {
        const double Fn_ = Fn * cdata.area_ratio;
        i_forces.delta_F[0] = Fn_ * cdata.en[0];
        i_forces.delta_F[1] = Fn_ * cdata.en[1];
        i_forces.delta_F[2] = Fn_ * cdata.en[2];
      } else {
        i_forces.delta_F[0] = cdata.Fn * cdata.en[0];
        i_forces.delta_F[1] = cdata.Fn * cdata.en[1];
        i_forces.delta_F[2] = cdata.Fn * cdata.en[2];

        j_forces.delta_F[0] = -i_forces.delta_F[0];
        j_forces.delta_F[1] = -i_forces.delta_F[1];
        j_forces.delta_F[2] = -i_forces.delta_F[2];
      }
    }

    inline void noCollision(ContactData & cdata, ForceData&, ForceData&)
    {
      if(cdata.touch) *cdata.touch &= ~TOUCH_NORMAL_MODEL;
      double * const shear = &cdata.contact_history[history_offset];
      shear[0] = 0.0;
    }

    void beginPass(CollisionData&, ForceData&, ForceData&){}
    void endPass(CollisionData&, ForceData&, ForceData&){}

  protected:
    double **kn2k2Max;
    double **kn2kc;
    double **phiF;
    int history_offset;
  };
}
#endif // NORMAL_MODEL_HOOKE_HYSTERESIS_H_
#endif
