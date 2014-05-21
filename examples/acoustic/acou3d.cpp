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
#include "vecmatop.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>

void SetupDistrib(int total_points, std::vector<int>& distrib) {
#ifndef RELEASE
    CallStackEntry entry("SetupDistrib");
#endif
    int mpisize = getMPISize();
    distrib.resize(mpisize + 1);
    int avg_points = total_points / mpisize;
    int extra = total_points - avg_points * mpisize;
    distrib[0] = 0;
    for (int i = 1; i < distrib.size(); ++i) {
        distrib[i] = distrib[i - 1] + avg_points;
        if (i - 1 < extra) {
            distrib[i] += 1;
        }
    }
}

int Acoustic3d::setup(vector<Point3>& vertvec, vector<Index3>& facevec,
                      Point3 ctr, int accu) {
#ifndef RELEASE
    CallStackEntry entry("Acoustic3d::setup");
#endif
    _vertvec = vertvec;
    _facevec = facevec;
    _ctr = ctr;
    _accu = accu;
    // Compute the diagonal scaling.
    TrMesh trmesh;
    SAFE_FUNC_EVAL(trmesh.setup(vertvec, facevec));
    SAFE_FUNC_EVAL(trmesh.compute_interior(_diavec));
    for (int k = 0; k < _diavec.size(); ++k) {
        _diavec[k] /= (4*M_PI);
    }
    SAFE_FUNC_EVAL( trmesh.compute_area(_arevec) );
    // Load the quadrature weights
    vector<int> all(1,1);
    std::ifstream gin("gauwgts.bin");
    CHECK_TRUE_MSG(!gin.fail(), "Could not open gauwgts.bin");
    SAFE_FUNC_EVAL( deserialize(_gauwgts, gin, all) );
    int mpirank = getMPIRank();
    if (mpirank == 0) {
        std::cerr << "gauwgts size " << _gauwgts.size() << std::endl;
    }
    std::ifstream lin("sigwgts.bin");
    CHECK_TRUE_MSG(!lin.fail(), "Could not open sigwgts.bin");
    SAFE_FUNC_EVAL( deserialize(_sigwgts, lin, all) );
    if (mpirank == 0) {
        std::cerr << "sigwgts size " << _sigwgts.size() << std::endl;
    }
    return 0;
}

bool Acoustic3d::Own(int index, int mpirank) {
#ifndef RELEASE
    CallStackEntry entry("Acoustic3d::Own");
#endif
    return index >= _dist[mpirank] && index < _dist[mpirank + 1];
}

int Acoustic3d::InitializeData(std::map<std::string, std::string>& opts) {
#ifndef RELEASE
    CallStackEntry entry("Acoustic3d::eval");
#endif
    int mpirank, mpisize;
    getMPIInfo(&mpirank, &mpisize);

    CHECK_TRUE_MSG(_gauwgts.find(5) != _gauwgts.end(),
		   "Problem with quadrature weights");
    DblNumMat& gauwgt = _gauwgts[5];
    int num_quad_points = gauwgt.m();
    std::vector<Point3> posvec;
    std::vector<Point3> norvec;
    
    // Setup the distribution of points.
    SetupDistrib(_facevec.size() * num_quad_points + _vertvec.size(), _dist);

    for (int fi = 0; fi < _facevec.size(); ++fi) {
        // Only read if there is a chance this process will own.
	// TODO(arbenson): add a check here.
	Index3& face = _facevec[fi];
	// Get the three vertices of the face.
	Point3 pos0 = _vertvec[face(0)];
	Point3 pos1 = _vertvec[face(1)];
	Point3 pos2 = _vertvec[face(2)];
	
	Point3 nor = cross(pos1 - pos0, pos2 - pos0);
	nor = nor / nor.l2();
	
	for (int gi = 0; gi < num_quad_points; ++gi) {
            if (!Own(fi * num_quad_points + gi, mpirank)) {
                continue;
	    }
	    double loc0 = gauwgt(gi, 0);
	    double loc1 = gauwgt(gi, 1);
	    double loc2 = gauwgt(gi, 2);
	    double wgt  = gauwgt(gi, 3);
	    posvec.push_back(loc0 * pos0 + loc1 * pos1 + loc2 * pos2);
	    norvec.push_back(nor);
	}
    }
    int num_faces = _facevec.size();
    for (int vi = 0; vi < _vertvec.size(); ++vi) {
        if (!Own(num_faces * num_quad_points + vi, mpirank)) {
            continue;
	}
	posvec.push_back(_vertvec[vi]);
	norvec.push_back(_vertvec[vi]);  // dummy
    }

    MPI_Barrier(MPI_COMM_WORLD);
    if (mpirank == 0) {
	std::cout << "putting in ownerinfo" << std::endl;
    }

    // Positions, densities, potentials, and normals all follow this partitioning.
    ParVec<int, Point3, PtPrtn>& positions = _wave._positions;
    ParVec<int, Point3, PtPrtn>& normal_vecs = _wave._normal_vecs;
    positions.prtn().ownerinfo() = _dist;
    normal_vecs.prtn().ownerinfo() = _dist;
    
    int start_ind = _dist[mpirank];
    for (int i = 0; i < posvec.size(); ++i) {
	positions.insert(start_ind + i, posvec[i]);
    }
    for (int i = 0; i < norvec.size(); ++i) {
	normal_vecs.insert(start_ind + i, norvec[i]);
    }
    
    _wave._ctr = _ctr;
    _wave._ACCU = _accu;
    _wave._kernel = Kernel3d(KERNEL_HELM_MIXED);
    _wave._equiv_kernel = Kernel3d(KERNEL_HELM);
    
    // Deal with geometry partition.  For now, we just do a cyclic partition.
    // TODO(arbenson): be more clever about the partition.
    int num_levels = ceil(log(sqrt(_K)) / log(2));
    int num_cells = pow2(num_levels);
    _wave._geomprtn.resize(num_cells, num_cells, num_cells);
    int curr_proc = 0;
    for (int k = 0; k < num_cells; ++k) {
	for (int j = 0; j < num_cells; ++j) {
	    for (int i = 0; i < num_cells; ++i) {
		_wave._geomprtn(i, j, k) = curr_proc;
		curr_proc = (curr_proc + 1) % mpisize;
	    }
	}
    }
    
    Mlib3d& mlib = _wave._mlib;
    mlib._NPQ = _wave._NPQ;
    mlib._kernel = Kernel3d(KERNEL_HELM);
    mlib.setup(opts);
    
    _wave.setup(opts);
    return 0;
}

int Acoustic3d::Apply(ParVec<int, cpx, PtPrtn>& in, ParVec<int, cpx, PtPrtn>& out) {
#ifndef RELEASE
    CallStackEntry entry("Acoustic3d::Apply");
#endif
    int mpirank, mpisize;
    getMPIInfo(&mpirank, &mpisize);
    CHECK_TRUE_MSG(_gauwgts.find(5) != _gauwgts.end(),
                   "Problem with quadrature weights");
    DblNumMat& gauwgt = _gauwgts[5];
    CHECK_TRUE_MSG(_sigwgts.find(5) != _gauwgts.end(),
                   "Problem with singularity weights");

    int num_quad_points = gauwgt.m();
    int num_faces = _facevec.size();
    int num_vertices = _vertvec.size();
    std::vector<cpx> denvec;

    int start_face_ind = _dist[mpirank] / num_quad_points;
    int end_face_ind = std::min(_dist[mpirank + 1] / num_quad_points + 1,
                                static_cast<int>(_facevec.size()));
    std::vector<int> req_keys;
    for (int fi = start_face_ind; fi < end_face_ind; ++fi) {
        Index3& ind = _facevec[fi];
        req_keys.push_back(ind(0));
        req_keys.push_back(ind(1));
        req_keys.push_back(ind(2));
    }
    std::vector<int> all(1, 1);
    in.getBegin(req_keys, all);
    in.getEnd(req_keys);

    // 1. Scale input with gaussian quadrature.
    for (int fi = start_face_ind; fi < end_face_ind; ++fi) {
        // Only handle if this process needs this data.
        Index3& ind = _facevec[fi];
        double are = _arevec[fi];
        cpx den0 = in.access(ind(0));
        cpx den1 = in.access(ind(1));
        cpx den2 = in.access(ind(2));
        for (int gi = 0; gi < num_quad_points; ++gi) {
            if (!Own(fi * num_quad_points + gi, mpirank)) {
                continue;
            }
            double loc0 = gauwgt(gi, 0);
            double loc1 = gauwgt(gi, 1);
            double loc2 = gauwgt(gi, 2);
            double wgt  = gauwgt(gi, 3);
            denvec.push_back((loc0 * den0 + loc1 * den1 + loc2 * den2) * (are * wgt));
        }
    }
    // Add zero densities at the vertices.
    for (int vi = 0; vi < _vertvec.size(); ++vi) {
        if (!Own(num_faces * num_quad_points + vi, mpirank)) {
            continue;
        }
        denvec.push_back(cpx(0, 0));
    }

    ParVec<int, cpx, PtPrtn> densities;
    densities.prtn().ownerinfo() = _dist;
    ParVec<int, cpx, PtPrtn> potentials;
    potentials.prtn().ownerinfo() = _dist;
    int start_ind = _dist[mpirank];
    for (int i = 0; i < denvec.size(); ++i) {
        densities.insert(start_ind + i, denvec[i]);
    }
    cpx dummy_val(0, 0);
    for (int i = 0; i < denvec.size(); ++i) {
        potentials.insert(start_ind + i, dummy_val);
    }

    // 2. Call directional FMM
    SAFE_FUNC_EVAL(_wave.eval(densities, potentials));
    IntNumVec check_keys(mpisize);
    for (int i = 0; i < mpisize; ++i) {
        int ind = (_dist[i] + _dist[i + 1]) / 2;
        check_keys(i) = ind;
    }
    _wave.check(densities, potentials, check_keys);

    // 3. Handle angle
    // First, communicate data.
    std::vector<int> in_reqs, potential_reqs;
    for (int i = 0; i < _vertvec.size(); ++i) {
        if (out.prtn().owner(i) == mpirank) {
            in_reqs.push_back(i);
            potential_reqs.push_back(num_faces * num_quad_points + i);
        }
    }
    in.getBegin(in_reqs, all);
    potentials.getBegin(potential_reqs, all);
    in.getEnd(in_reqs);
    potentials.getEnd(potential_reqs);
    
    for (int i = 0; i < _vertvec.size(); ++i) {
        // Add TODO(arbenson): what is this doing?
        if (out.prtn().owner(i) == mpirank) {
            cpx diag = _diavec[i] * in.access(i);
            cpx potential = potentials.access(num_faces * num_quad_points + i);
            cpx val = diag + potential;
            out.insert(i, val);
        }
    }

    // 4. Remove nearby from the output
    RemoveNearby(in, out, densities);

    // 5. Visit faces and add singularity correction.
    SingularityCorrection(in, out);

    return 0;
}

void Acoustic3d::RemoveNearby(ParVec<int, cpx, PtPrtn>& in, ParVec<int, cpx, PtPrtn>& out,
                              ParVec<int, cpx, PtPrtn>& densities) {
#ifndef RELEASE
    CallStackEntry entry("Acoustic3d::RemoveNearby");
#endif
    int mpirank = getMPIRank();
    DblNumMat& gauwgt = _gauwgts[5];
    CHECK_TRUE_MSG(_sigwgts.find(5) != _gauwgts.end(),
                   "Problem with singularity weights");
    int num_quad_points = gauwgt.m();

    // Communicate data
    std::vector<int> req_keys;
    for (int fi = 0; fi < _facevec.size(); fi++) {
        Index3& ind = _facevec[fi];
        if (out.prtn().owner(ind(0)) == mpirank || 
            out.prtn().owner(ind(1)) == mpirank || 
            out.prtn().owner(ind(2)) == mpirank) {
            for (int i = 0; i < num_quad_points; ++i) {
                req_keys.push_back(fi * num_quad_points + i);
            }
        }
    }
    std::vector<int> all(1, 1);
    densities.getBegin(req_keys, all);
    densities.getEnd(req_keys);

    // We deal with the singularity directly, so we remove contributions from
    // adjacent triangles.
    for (int fi = 0; fi < _facevec.size(); fi++) {
        Index3& ind = _facevec[fi];
        if (out.prtn().owner(ind(0)) != mpirank &&
            out.prtn().owner(ind(1)) != mpirank && 
            out.prtn().owner(ind(2)) != mpirank) {
            continue;
        }
        // Get the three vertices of the face.
        Point3 pos0 = _vertvec[ind(0)];
        Point3 pos1 = _vertvec[ind(1)];
        Point3 pos2 = _vertvec[ind(2)];

        Point3 nor = cross(pos1 - pos0, pos2 - pos0);
        nor = nor / nor.l2();

        // Positions at this face
        DblNumMat srcpos(3, num_quad_points);
        // Normal vectors at this face
        DblNumMat srcnor(3, num_quad_points);

        for (int gi = 0; gi < num_quad_points; ++gi) {
            double loc0 = gauwgt(gi, 0);
            double loc1 = gauwgt(gi, 1);
            double loc2 = gauwgt(gi, 2);
            double wgt  = gauwgt(gi, 3);
            Point3 pos = loc0 * pos0 + loc1 * pos1 + loc2 * pos2;
            for (int i = 0; i < 3; ++i) {
                srcpos(i, gi) = pos(i);
                srcnor(i, gi) = nor(i);
            }
        }

        // Positions at the vertices.
        vector<Point3> trgpostmp;
        trgpostmp.push_back(_vertvec[ind(0)]);
        trgpostmp.push_back(_vertvec[ind(1)]);
        trgpostmp.push_back(_vertvec[ind(2)]);
        DblNumMat trgpos(3, 3, false, (double*)(&(trgpostmp[0])));
        // Values at this face from FMM.
        CpxNumVec srcden(num_quad_points);
        for (int i = 0; i < num_quad_points; ++i) {
            srcden(i) = densities.access(fi * num_quad_points + i);
        }
        CpxNumVec trgval(3);
        CpxNumMat mat;
        _wave._kernel.kernel(trgpos, srcpos, srcnor, mat);
        zgemv(1.0, mat, srcden, 0.0, trgval);
      
        for (int i = 0; i < 3; ++i) {
            if (out.prtn().owner(ind(i)) == mpirank) {
                cpx val = out.access(ind(i)) - trgval(i);
                out.insert(ind(i), val);
            }
        }
    }
}

void Acoustic3d::SingularityCorrection(ParVec<int, cpx, PtPrtn>& in,
                                       ParVec<int, cpx, PtPrtn>& out) {
#ifndef RELEASE
    CallStackEntry entry("Acoustic3d::SingularityCorrection");
#endif
    int mpirank = getMPIRank();
    DblNumMat& sigwgt = _sigwgts[5];
    int numsig = sigwgt.m();

    // Communicate the data that we need.
    std::vector<int> req_keys;
    for (int fi = 0; fi < _facevec.size(); ++fi) {
        Index3& ind = _facevec[fi];
        // If we own any of the three indices, then we need them all.
        if (out.prtn().owner(ind(0)) == mpirank || 
            out.prtn().owner(ind(1)) == mpirank || 
            out.prtn().owner(ind(2)) == mpirank) {
            req_keys.push_back(ind(0));
            req_keys.push_back(ind(1));
            req_keys.push_back(ind(2));
        }
    }
    std::vector<int> all(1, 1);
    in.getBegin(req_keys, all);
    in.getEnd(req_keys);
      
    for (int fi = 0; fi < _facevec.size(); ++fi) {
        Index3& ind = _facevec[fi];
        // If I do not own any part of the face, then we skip.
        if (out.prtn().owner(ind(0)) != mpirank &&
            out.prtn().owner(ind(1)) != mpirank && 
            out.prtn().owner(ind(2)) != mpirank) {
            continue;
        }
        Point3 pos0 = _vertvec[ind(0)];
        Point3 pos1 = _vertvec[ind(1)];
        Point3 pos2 = _vertvec[ind(2)];
        double are = _arevec[fi];
        Point3 nor = cross(pos1 - pos0, pos2 - pos0); 
        nor = nor / nor.l2();
        cpx den0 = in.access(ind(0));
        cpx den1 = in.access(ind(1));
        cpx den2 = in.access(ind(2));

        for (int corner = 0; corner < 3; ++corner) {
            vector<Point3> srcpostmp;
            vector<Point3> srcnortmp;
            vector<Point3> trgpostmp;
            vector<cpx> srcdentmp;
            for (int li = 0; li < numsig; ++li) {
                double loc0 = sigwgt(li, 0);
                double loc1 = sigwgt(li, 1);
                double loc2 = sigwgt(li, 2);
                double wgt  = sigwgt(li, 3);
                Point3 pos;
                cpx den;
                if (corner == 0) {
                    Point3 pos = loc0 * pos0 + loc1 * pos1 + loc2 * pos2;
                    cpx den = (loc0 * den0 + loc1 * den1 + loc2 * den2) * (are * wgt);
                } else if (corner == 1) {
                    Point3 pos = loc0 * pos1 + loc1 * pos2 + loc2 * pos0;
                    cpx den = (loc0 * den1 + loc1 * den2 + loc2 * den0) * (are * wgt);
                } else if (corner == 2) {
                    Point3 pos = loc0 * pos2 + loc1 * pos0 + loc2 * pos1;
                    cpx den = (loc0 * den2 + loc1 * den0 + loc2 * den1) * (are * wgt);
                }
                srcpostmp.push_back(pos);
                srcnortmp.push_back(nor);
                srcdentmp.push_back(den);
            }
            if (corner == 0) {
                trgpostmp.push_back(pos0);
            } else if (corner == 1) {
                trgpostmp.push_back(pos1);
            } else if (corner == 2) {
                trgpostmp.push_back(pos2);
            }
            DblNumMat srcpos(3, numsig, false, (double*)(&(srcpostmp[0])));
            DblNumMat srcnor(3, numsig, false, (double*)(&(srcnortmp[0])));
            DblNumMat trgpos(3, 1, false, (double*)(&(trgpostmp[0])));
            CpxNumVec srcden(numsig, false, (cpx*)(&(srcdentmp[0])));
            CpxNumVec trgval(1);
            CpxNumMat mat;
            _wave._kernel.kernel(trgpos, srcpos, srcnor, mat);
            zgemv(1.0, mat, srcden, 0.0, trgval);
            if (out.prtn().owner(ind(corner)) == mpirank) {
                cpx val = out.access(ind(corner)) + trgval(0);
                out.insert(ind(corner), val);
            }
        }
    }
}

// Apply the operator to x and store the result in y
void Acoustic3d::Apply(CpxNumVec&x, CpxNumVec& y) {
    int mpirank = getMPIRank();

    // Set up the parvec structures.
    ParVec<int, cpx, PtPrtn> in, out;
    in.prtn().ownerinfo() = _vert_distrib;
    out.prtn().ownerinfo() = _vert_distrib;
    
    int start_index = _vert_distrib[mpirank];
    CHECK_TRUE(start_index + x.m() == _vert_distrib[mpirank + 1]);
    
    for (int i = 0; i < x.m(); ++i) {
        cpx val = x(i);
        in.insert(i + start_index, val);
        out.insert(i + start_index, val);  // dummy
    }

    // Call the function.
    Apply(in, out);

    // Store the result in y
    CHECK_TRUE(x.m() == y.m());
    for (int i = 0; i < y.m(); ++i) {
        y(i) = out.access(i + start_index);
    }
}

void Acoustic3d::Run(std::map<std::string, std::string>& opts) {
#ifndef RELEASE
    CallStackEntry entry("Acoustic3d::Run");
#endif
    InitializeData(opts);
    SetupDistrib(_vertvec.size(), _vert_distrib);

    // Random entries for now.
    // TODO (arbenson): change this when real input is taken.
    int mpirank = getMPIRank();

    int m = _vert_distrib[mpirank + 1] - _vert_distrib[mpirank];
    // Initialize starting guess to 0.
    CpxNumVec x0(m);
    setvalue(x0, cpx(0.0));

    // Initialize right-hand-side (random for now)
    CpxNumVec b(m);
    for (int i = 0; i < b.m(); ++i) {
        double real = static_cast<double>(rand()) / RAND_MAX;
        double imag = static_cast<double>(rand()) / RAND_MAX;
        cpx val(real, imag);
        b(i) = val;
    }
    double tol = 1e-4;
    int max_iter = 30;
    auto apply_func = [this] (CpxNumVec& x, CpxNumVec& y) { Apply(x, y); };
    GMRES(b, x0, apply_func, tol, max_iter);
}
