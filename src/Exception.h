//=============================================================================
//  Exception.h
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


#ifndef _EXCEPTION_H_
#define _EXCEPTION_H_

#include <cstdlib>
#include <iostream>
#include <string>
using namespace std;


enum RunType {cplusplus, python};

class GandalfError {
public:
    GandalfError(std::string msgaux){msg = msgaux;}
    ~GandalfError() {}
    std::string msg;
};

class StopError: public GandalfError {
public:
  StopError(std::string msgaux):
    GandalfError(msgaux)
    {};
};

class ExceptionHandler {
private:
  ExceptionHandler(RunType runtypeaux) : runtype(runtypeaux), mpi(0) {}
  static ExceptionHandler * istance;
  const RunType runtype;
  int mpi;
public:
  void raise(string msg);
  static void makeExceptionHandler (RunType runtypeaux);
  static ExceptionHandler & getIstance() {return *istance;}
  static void set_mpi(int mpi_aux) {if (istance) istance->mpi=mpi_aux;}
};

#endif // _EXCEPTION_H_
