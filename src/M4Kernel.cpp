//=============================================================================
//  M4Kernel.cpp
//  ..
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


#include <math.h>
#include <iostream>
#include "Constants.h"
#include "SphKernel.h"
using namespace std;



//=============================================================================
// M4Kernel::M4Kernel
//=============================================================================
template <int ndim>
M4Kernel<ndim>::M4Kernel(string kernelname):
  SphKernel<ndim>()
{
  this->kernrange = (FLOAT) 2.0;
  this->invkernrange = (FLOAT) 0.5;
  this->kernrangesqd = (FLOAT) 4.0;
  if (ndim == 1) this->kernnorm = twothirds;
  else if (ndim == 2) this->kernnorm = invpi*(FLOAT) (10.0/7.0);
  else if (ndim == 3) this->kernnorm = invpi;
}



//=============================================================================
// M4Kernel::~M4Kernel
//=============================================================================
template <int ndim>
M4Kernel<ndim>::~M4Kernel()
{
}



template class M4Kernel<1>;
template class M4Kernel<2>;
template class M4Kernel<3>;
