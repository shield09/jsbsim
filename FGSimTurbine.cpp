/*%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

 Module:       FGSimTurbine.cpp
 Author:       David Culp
 Date started: 03/11/2003
 Purpose:      This module models a turbine engine.

 ------------- Copyright (C) 2003  David Culp (davidculp2@comcast.net) ---------

 This program is free software; you can redistribute it and/or modify it under
 the terms of the GNU General Public License as published by the Free Software
 Foundation; either version 2 of the License, or (at your option) any later
 version.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 details.

 You should have received a copy of the GNU General Public License along with
 this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 Place - Suite 330, Boston, MA  02111-1307, USA.

 Further information about the GNU General Public License can also be found on
 the world wide web at http://www.gnu.org.

FUNCTIONAL DESCRIPTION
--------------------------------------------------------------------------------

This class descends from the FGEngine class and models a Turbine engine based
on parameters given in the engine config file for this class

HISTORY
--------------------------------------------------------------------------------
03/11/2003  DPC  Created
09/08/2003  DPC  Changed Calculate() and added engine phases 

%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
INCLUDES
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%*/

#include <vector>
#include "FGSimTurbine.h"

namespace JSBSim {

static const char *IdSrc = "$Id: FGSimTurbine.cpp,v 1.9 2003/10/18 13:21:25 ehofman Exp $";
static const char *IdHdr = ID_SIMTURBINE;

/*%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
CLASS IMPLEMENTATION
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%*/


FGSimTurbine::FGSimTurbine(FGFDMExec* exec, FGConfigFile* cfg) : FGEngine(exec)
{
  SetDefaults();
  FGEngine::Type=etSimTurbine;
  Load(cfg);
  Debug(0);
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

FGSimTurbine::~FGSimTurbine()
{
  Debug(1);
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// The main purpose of Calculate() is to determine what phase the engine should
// be in, then call the corresponding function. 

double FGSimTurbine::Calculate(double dummy)
{
  TAT = (Auxiliary->GetTotalTemperature() - 491.69) * 0.5555556;
  dt = State->Getdt() * Propulsion->GetRate();
  ThrottleCmd = FCS->GetThrottleCmd(EngineNumber);

  // When trimming is finished check if user wants engine OFF or RUNNING
  if ((phase == tpTrim) && (dt > 0)) {
    if (Running && !Starved) {
      phase = tpRun;
      N2 = IdleN2;
      N1 = IdleN1;
      OilTemp_degK = TAT + 10;  
      Cutoff = false;
      }
    else {
      phase = tpOff;
      Cutoff = true;
      EGT_degC = TAT; 
      }
    }
  
  if (!Running && Cutoff && Starter) {
     if (phase == tpOff) phase = tpSpinUp;
     }
  if (!Running && !Cutoff && (N2 > 15.0)) phase = tpStart;
  if (Cutoff && (phase != tpSpinUp)) phase = tpOff;
  if (dt == 0) phase = tpTrim;
  if (Starved) phase = tpOff;
  if (Stalled) phase = tpStall;
  if (Seized) phase = tpSeize;
  
  switch (phase) {
    case tpOff:    Thrust = Off(); break;
    case tpRun:    Thrust = Run(); break;
    case tpSpinUp: Thrust = SpinUp(); break;
    case tpStart:  Thrust = Start(); break;
    case tpStall:  Thrust = Stall(); break;
    case tpSeize:  Thrust = Seize(); break;
    case tpTrim:   Thrust = Trim(); break;
    default: Thrust = Off();
  }   

  return Thrust;  
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

double FGSimTurbine::Off(void)
{
    double qbar = Translation->Getqbar();
    Running = false;
    FuelFlow_pph = 0.0;
    N1 = Seek(&N1, qbar/10.0, N1/2.0, N1/2.0);
    N2 = Seek(&N2, qbar/15.0, N2/2.0, N2/2.0);
    EGT_degC = Seek(&EGT_degC, TAT, 11.7, 7.3);
    OilTemp_degK = Seek(&OilTemp_degK, TAT + 273.0, 0.2, 0.2);  
    OilPressure_psi = N2 * 0.62;
    EPR = 1.0;
    return 0.0; 
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

double FGSimTurbine::Run(void)
{
    double idlethrust, milthrust, thrust;
    double N2norm;   // 0.0 = idle N2, 1.0 = maximum N2
    idlethrust = MilThrust * ThrustTables[0]->TotalValue();
    milthrust = (MilThrust - idlethrust) * ThrustTables[1]->TotalValue();
 
    Running = true;
    Starter = false;
   
    N2 = Seek(&N2, IdleN2 + ThrottleCmd * N2_factor, delay, delay * 3.0);
    N1 = Seek(&N1, IdleN1 + ThrottleCmd * N1_factor, delay, delay * 2.4);
    N2norm = (N2 - IdleN2) / N2_factor;
    thrust = idlethrust + (milthrust * N2norm * N2norm); 
    thrust = thrust * (1.0 - BleedDemand);
    FuelFlow_pph = thrust * TSFC;
    if (FuelFlow_pph < IdleFF) FuelFlow_pph = IdleFF;
    EGT_degC = TAT + 363.1 + ThrottleCmd * 357.1;
    OilPressure_psi = N2 * 0.62;
    OilTemp_degK = Seek(&OilTemp_degK, 366.0, 1.2, 0);
    EPR = 1.0 + thrust/MilThrust;
    NozzlePosition = Seek(&NozzlePosition, 1.0 - N2norm, 0.8, 0.8);
    if (Reversed) thrust = thrust * -0.2;

    if (AugMethod == 1) {
      if ((ThrottleCmd > 0.99) && (N2 > 97.0)) {Augmentation = true;} 
        else {Augmentation = false;}
      }

    if ((Augmented == 1) && Augmentation) {
      thrust = MaxThrust * ThrustTables[2]->TotalValue();
      FuelFlow_pph = thrust * ATSFC;
      NozzlePosition = Seek(&NozzlePosition, 1.0, 0.8, 0.8);
      }

    if ((Injected == 1) && Injection)
      thrust = thrust * ThrustTables[3]->TotalValue(); 

    ConsumeFuel();
    if (Cutoff) phase = tpOff;
    if (Starved) phase = tpOff;
    return thrust;
}
        
//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

double FGSimTurbine::SpinUp(void)
{
    Running = false;
    FuelFlow_pph = 0.0;
    N2 = Seek(&N2, 25.18, 3.0, N2/2.0);
    N1 = Seek(&N1, 5.21, 1.0, N1/2.0);
    EGT_degC = TAT;
    OilPressure_psi = N2 * 0.62;
    OilTemp_degK = TAT + 273.0;
    EPR = 1.0;
    NozzlePosition = 1.0;
    return 0.0;
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

double FGSimTurbine::Start(void)
{
    if ((N2 > 15.0) && !Starved) {       // minimum 15% N2 needed for start
      Cranking = true;                   // provided for sound effects signal
      if (N2 < IdleN2) {
        N2 = Seek(&N2, IdleN2, 2.0, N2/2.0);
        N1 = Seek(&N1, IdleN1, 1.4, N1/2.0);
        EGT_degC = Seek(&EGT_degC, TAT + 363.1, 21.3, 7.3);
        FuelFlow_pph = Seek(&FuelFlow_pph, IdleFF, 103.7, 103.7);
        OilPressure_psi = N2 * 0.62;
        }
      else {
        phase = tpRun;
        Running = true;
        Starter = false;
        Cranking = false;
        } 
      }
    else {                 // no start if N2 < 15%
      phase = tpOff;
      Starter = false;
      }
    return 0.0; 
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

double FGSimTurbine::Stall(void)
{
    double qbar = Translation->Getqbar();
    EGT_degC = TAT + 903.14;
    FuelFlow_pph = IdleFF;
    N1 = Seek(&N1, qbar/10.0, 0, N1/10.0); 
    N2 = Seek(&N2, qbar/15.0, 0, N2/10.0);
    if (ThrottleCmd == 0) phase = tpRun;        // clear the stall with throttle
    return 0.0; 
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

double FGSimTurbine::Seize(void)
{
    double qbar = Translation->Getqbar();
    N2 = 0.0;
    N1 = Seek(&N1, qbar/20.0, 0, N1/15.0);
    FuelFlow_pph = IdleFF;
    OilPressure_psi = 0.0;
    OilTemp_degK = Seek(&OilTemp_degK, TAT + 273.0, 0, 0.2);
    Running = false;
    return 0.0; 
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

double FGSimTurbine::Trim(void)
{
    double idlethrust, milthrust, thrust;
    idlethrust = MilThrust * ThrustTables[0]->TotalValue();
    milthrust = (MilThrust - idlethrust) * ThrustTables[1]->TotalValue();
    thrust = idlethrust + (milthrust * ThrottleCmd * ThrottleCmd);
    return thrust; 
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

double FGSimTurbine::CalcFuelNeed(void)
{
  return FuelFlow_pph /3600 * State->Getdt() * Propulsion->GetRate();
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

double FGSimTurbine::GetPowerAvailable(void) {
  if( ThrottleCmd <= 0.77 )
    return 64.94*ThrottleCmd;
  else
    return 217.38*ThrottleCmd - 117.38;
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

double FGSimTurbine::Seek(double *var, double target, double accel, double decel) {
  double v = *var;
  if (v > target) {
    v -= dt * decel;
    if (v < target) v = target;
  } else if (v < target) {
    v += dt * accel;
    if (v > target) v = target;
  }
  return v;
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

void FGSimTurbine::SetDefaults(void)
{
  Name = "None_Defined";
  MilThrust = 10000.0;
  MaxThrust = 10000.0;
  BypassRatio = 0.0;
  TSFC = 0.8;
  ATSFC = 1.7;
  IdleN1 = 30.0;
  IdleN2 = 60.0;
  MaxN1 = 100.0;
  MaxN2 = 100.0;
  Augmented = 0;
  AugMethod = 0;
  Injected = 0;
  BleedDemand = 0.0;
  ThrottleCmd = 0.0;
  InletPosition = 1.0;
  NozzlePosition = 1.0;
  Augmentation = false;
  Injection = false;
  Reversed = false;
  phase = tpOff;
  Stalled = false;
  Seized = false;
  Overtemp = false;
  Fire = false;
}

//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

bool FGSimTurbine::Load(FGConfigFile *Eng_cfg)
{
  int i;
  string token;
  Name = Eng_cfg->GetValue("NAME");
  cout << Name << endl;
  Eng_cfg->GetNextConfigLine();
  *Eng_cfg >> token >> MilThrust;
  *Eng_cfg >> token >> MaxThrust;
  *Eng_cfg >> token >> BypassRatio;
  *Eng_cfg >> token >> TSFC;
  *Eng_cfg >> token >> ATSFC;
  *Eng_cfg >> token >> IdleN1;
  *Eng_cfg >> token >> IdleN2;
  *Eng_cfg >> token >> MaxN1;
  *Eng_cfg >> token >> MaxN2;
  *Eng_cfg >> token >> Augmented;
  *Eng_cfg >> token >> AugMethod;
  *Eng_cfg >> token >> Injected;
  i=0;
  while( Eng_cfg->GetValue() != string("/FG_SIMTURBINE") && i < 10){
    ThrustTables.push_back( new FGCoefficient(FDMExec) );
    ThrustTables.back()->Load(Eng_cfg);
    i++;
  }
  
  // pre-calculations and initializations
  delay= 60.0 / (BypassRatio + 3.0);
  N1_factor = MaxN1 - IdleN1;
  N2_factor = MaxN2 - IdleN2;
  OilTemp_degK = (Auxiliary->GetTotalTemperature() - 491.69) * 0.5555556 + 273.0;
  IdleFF = pow(MilThrust, 0.2) * 107.0;  // just an estimate
  return true;
}


//%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
//    The bitmasked value choices are as follows:
//    unset: In this case (the default) JSBSim would only print
//       out the normally expected messages, essentially echoing
//       the config files as they are read. If the environment
//       variable is not set, debug_lvl is set to 1 internally
//    0: This requests JSBSim not to output any messages
//       whatsoever.
//    1: This value explicity requests the normal JSBSim
//       startup messages
//    2: This value asks for a message to be printed out when
//       a class is instantiated
//    4: When this value is set, a message is displayed when a
//       FGModel object executes its Run() method
//    8: When this value is set, various runtime state variables
//       are printed out periodically
//    16: When set various parameters are sanity checked and
//       a message is printed out when they go out of bounds

void FGSimTurbine::Debug(int from)
{
  if (debug_lvl <= 0) return;

  if (debug_lvl & 1) { // Standard console startup message output
    if (from == 0) { // Constructor

    }
  }
  if (debug_lvl & 2 ) { // Instantiation/Destruction notification
    if (from == 0) cout << "Instantiated: FGSimTurbine" << endl;
    if (from == 1) cout << "Destroyed:    FGSimTurbine" << endl;
  }
  if (debug_lvl & 4 ) { // Run() method entry print for FGModel-derived objects
  }
  if (debug_lvl & 8 ) { // Runtime state variables
  }
  if (debug_lvl & 16) { // Sanity checking
  }
  if (debug_lvl & 64) {
    if (from == 0) { // Constructor
      cout << IdSrc << endl;
      cout << IdHdr << endl;
    }
  }
}
}