/* Distributed Directional Fast Multipole Method
   Copyright (C) 2014 Austin Benson, Lexing Ying, and Jack Poulson

 This file is part of DDFMM.

    DDFMM is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    DDFMM is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with DDFMM.  If not, see <http://www.gnu.org/licenses/>. */

#include "acou3d.hpp"
#include "serialize.hpp"
#include "trmesh.hpp"

#include <fstream>

//-----------------------------------
int Acoustic3D::setup(vector<Point3>& vertvec, vector<Index3>& facevec,
		  Point3 ctr, int accu, Kernel3d knlbie) {
    _vertvec = vertvec;
    _facevec = facevec;
    _ctr = ctr;
    _accu = accu;
    _knlbie = knlbie;
    std::cerr << "type " << _knlbie.type() <<std::endl;
    // Compute the diagonal scaling.
    TrMesh trmesh("");
    SAFE_FUNC_EVAL( trmesh.setup(vertvec, facevec) );
    SAFE_FUNC_EVAL( trmesh.compute_interior(_diavec) );
    for(int k=0; k<_diavec.size(); k++) {
        _diavec[k] /= (4*M_PI);
    }
    SAFE_FUNC_EVAL( trmesh.compute_area(_arevec) );
    // Load the quadrature weights
    vector<int> all(1,1);
    std::ifstream gin("gauwgts.bin");
    SAFE_FUNC_EVAL( deserialize(_gauwgts, gin, all) );
    std::cerr << "gauwgts size " << _gauwgts.size() << std::endl;
    std::ifstream lin("sigwgts.bin");
    SAFE_FUNC_EVAL( deserialize(_sigwgts, lin, all) );
    std::cerr << "sigwgts size " << _sigwgts.size() << std::endl;
    return 0;
}

//-----------------------------------
int Acoustic3D::eval(vector<Point3>& chk, vector<cpx>& den, vector<cpx>& val) {
  DblNumMat& gauwgt = _gauwgts[5];
  //
  int numgau = gauwgt.m();
  int NF = _facevec.size();
  _posvec.clear();
  _norvec.clear();
  vector<cpx> denvec;
  vector<cpx> valvec;

  int mpirank, mpisize;
  getMPIInfo(&mpirank, &mpisize);

  // Determine distribution of face vectors.
  int avg_faces = _facevec.size() / mpisize;
  int extra = _facevec.size() - avg_faces * mpisize;
  std::vector<int> dist(mpisize + 1);
  dist.resize(mpisize + 1);
  dist[0] = 0;
  for (int i = 1; i < dist.size(); ++i) {
      dist[i] = dist[i - 1] + avg_faces;
      if (i - 1 < extra) {
          dist[i] += 1;
      }
  }

  for (int fi = 0; fi < _facevec.size(); ++fi) {
      // Only read if this face is owned by this process.
      if (!(fi >= dist[mpirank] && fi < dist[mpirank + 1])) {
          continue;
      }
      Index3& face = _facevec[fi];
      // Get the three vertices of the face.
      Point3 pos0 = _vertvec[face(0)];
      Point3 pos1 = _vertvec[face(1)];
      Point3 pos2 = _vertvec[face(2)];

      double are = _arevec[fi];
      Point3 nor = cross(pos1 - pos0, pos2 - pos0);
      nor = nor / nor.l2();

      // Current density at the corners.
      cpx den0 = den[face(0)];
      cpx den1 = den[face(1)];
      cpx den2 = den[face(2)];

      for (int gi = 0; gi < numgau; ++gi) {
          double loc0 = gauwgt(gi, 0);
	  double loc1 = gauwgt(gi, 1);
	  double loc2 = gauwgt(gi, 2);
	  double wgt  = gauwgt(gi, 3);
	  _posvec.push_back(loc0 * pos0 + loc1 * pos1 + loc2 * pos2);
	  _norvec.push_back(nor);
	  denvec.push_back((loc0 * den0 + loc1 * den1 + loc2 * den2) * (are * wgt));
      }
  }
  // TODO (arbenson): put in check points?

  // Owners for the parvecs:
  std::vector<int> ownerinfo(mpisize + 1);
  ownerinfo[0] = 0;
  for (int i = 1; i < ownerinfo.size(); ++i) {
      int num_own = dist[mpirank + 1] - dist[mpirank];
      ownerinfo[i] = ownerinfo[i - 1] + num_own * numgau;
  }
  // Positions, densities, potentials, and normals all follow this partitioning.
  ParVec<int, Point3, PtPrtn>& positions = *_wave.posptr();
  positions.prtn().ownerinfo() = ownerinfo;
  _wave._normal_vecs.prtn().ownerinfo() = ownerinfo;

  ParVec<int, cpx, PtPrtn> densities;
  densities.prtn().ownerinfo() = ownerinfo;
  ParVec<int, cpx, PtPrtn> potentials;
  potentials.prtn().ownerinfo() = ownerinfo;

  int start_ind = ownerinfo[mpirank];
  for (int i = 0; i < _posvec.size(); ++i) {
      positions.insert(start_ind + i, _posvec[i]);
  }
  for (int i = 0; i < _norvec.size(); ++i) {
      _wave._normal_vecs.insert(start_ind + i, _norvec[i]);
  }
  for (int i = 0; i < denvec.size(); ++i) {
      densities.insert(start_ind + i, denvec[i]);
  }

  // TODO(arbenson): Setup kernel in _wave.
  _wave._ctr = _ctr;
  _wave._ACCU = _accu;

  // TODO(arbenson): make these options.
  _wave._K = 64;
  _wave._ptsmax = 80;
  _wave._maxlevel = 12;
  _wave._NPQ = 4;

  _wave.eval(densities, potentials);
  // TODO(arbenson): Put stuff into output vector.
  return 0;
}