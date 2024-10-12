// Copyright (c) 2010-2024, Lawrence Livermore National Security, LLC. Produced
// at the Lawrence Livermore National Laboratory. All Rights reserved. See files
// LICENSE and NOTICE for details. LLNL-CODE-806117.
//
// This file is part of the MFEM library. For more information and source code
// availability visit https://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the BSD-3 license. We welcome feedback and contributions, see file
// CONTRIBUTING.md for details.
//
//            ------------------------------------------------
//            Distance Miniapp: Finite element distance solver
//            ------------------------------------------------
//
// This miniapp computes the "distance" to a given point source or to the zero
// level set of a given function. Here "distance" refers to the length of the
// shortest path through the mesh. The input can be a DeltaCoefficient (for a
// point source), or any Coefficient (for the case of a level set). The output
// is a GridFunction that can be scalar (representing the scalar distance), or a
// vector (its magnitude is the distance, and its direction is the starting
// direction of the shortest path). The miniapp supports 3 solvers:
//
// 1. Heat solver:
//    K. Crane, C. Weischedel, M. Weischedel
//    Geodesics in Heat: A New Approach to Computing Distance Based on Heat Flow
//    ACM Transactions on Graphics, Vol. 32, No. 5, October, 2013
//
// 2. p-Laplacian solver:
//    A. Belyaev, P. Fayolle
//    On Variational and PDE-based Distance Function Approximations,
//    Computer Graphics Forum, 34: 104-118, 2015, Section 7.
//
// 3. Rvachev normalization solver: same paper as p-Laplacian, Section 6.
//    This solver is computationally cheap, but is accurate for distance
//    approximations only near the zero level set.
//
//
// The solution of the p-Laplacian solver approaches the signed distance when
// p->\infinity. Therefore, increasing p will improve the computed distance and,
// of course, will increase the computational cost.  The discretization of the
// p-Laplacian equation utilizes ideas from:
//
//    L. V. Kantorovich, V. I. Krylov
//    Approximate Methods of Higher Analysis, Interscience Publishers, Inc., 1958
//
//    J. Melenk, I. Babuska
//    The partition of unity finite element method: Basic theory and applications,
//    Computer Methods in Applied Mechanics and Engineering, 1996, 139, 289-314
//
// Resolving highly oscillatory input fields requires refining the mesh or
// increasing the order of the approximation. The above requirement for mesh
// resolution is independent of the conditions imposed on the mesh by the
// discretization of the actual distance solver. On the other hand, it is often
// enough to compute the distance field to a mean zero level of a smoothed
// version of the input field. In such cases, one can use a low-pass filter that
// removes the high-frequency content of the input field, such as the one in the
// class PDEFilter, based on the Screened Poisson equation. The radius specifies
// the minimal feature size in the filter output and, in the current example, is
// linked to the average mesh size. Theoretical description of the filter and
// discussion of the parameters can be found in:
//
//    B. S. Lazarov, O. Sigmund
//    Filters in topology optimization based on Helmholtz-type differential equations
//    International Journal for Numerical Methods in Engineering, 2011, 86, 765-781
//
// Compile with: make distance
//
// Sample runs:
//
//   Problem 0: point source.
//     mpirun -np 4 distance -m ./corners.mesh -p 0 -rs 3 -t 200.0
//
//   Problem 1: zero level set: ball at the center of the domain - the exact
// + distance is known, the code computes global and local errors.
//     mpirun -np 4 distance -m ../../data/inline-segment.mesh -rs 3 -o 2 -t 1.0 -p 1
//     mpirun -np 4 distance -m ../../data/inline-quad.mesh   -rs 3 -o 2 -t 1.0 -p 1
//     mpirun -np 4 distance -m ../../data/inline-hex.mesh -rs 1 -o 2 -p 1 -s 1
//
//   Problem 2: zero level set: perturbed sine
//     mpirun -np 4 distance -m ../../data/inline-quad.mesh -rs 3 -o 2 -t 1.0 -p 2
//     mpirun -np 4 distance -m ../../data/amr-quad.mesh    -rs 3 -o 2 -t 1.0 -p 2
//
//   Problem 3: level set: Gyroid
//     mpirun -np 4 distance -m ../../data/periodic-square.mesh -rs 5 -o 2 -t 1.0 -p 3
//     mpirun -np 4 distance -m ../../data/periodic-cube.mesh   -rs 3 -o 2 -t 1.0 -p 3 -s 2
//
//   Problem 4: level set: Union of doughnut and swiss cheese shapes
//     mpirun -np 4 distance -m ../../data/inline-hex.mesh -rs 3 -o 2 -t 1.0 -p 4

#include <fstream>
#include <iostream>
#include "../common/mfem-common.hpp"
#include "sbm_aux.hpp"

using namespace std;
using namespace mfem;
using namespace common;

real_t sine_ls(const Vector &x)
{
   const real_t sine = 0.25 * std::sin(4 * M_PI * x(0)) +
                       0.05 * std::sin(16 * M_PI * x(0));
   return (x(1) >= sine + 0.5) ? -1.0 : 1.0;
}

const real_t radius = 0.4;

real_t sphere_ls(const Vector &x)
{
   const int dim = x.Size();
   const real_t xc = x(0) - 0.5;
   const real_t yc = (dim > 1) ? x(1) - 0.5 : 0.0;
   const real_t zc = (dim > 2) ? x(2) - 0.5 : 0.0;
   const real_t r = sqrt(xc*xc + yc*yc + zc*zc);

   return (r >= radius) ? -1.0 : 1.0;
   //return xc >= 0.0 ? -1.0 : 1.0;
}

real_t exact_dist_sphere(const Vector &x)
{
   const int dim = x.Size();
   const real_t xc = x(0) - 0.5;
   const real_t yc = (dim > 1) ? x(1) - 0.5 : 0.0;
   const real_t zc = (dim > 2) ? x(2) - 0.5 : 0.0;
   const real_t r = sqrt(xc*xc + yc*yc + zc*zc);

   return fabs(r - radius);
}

class ExactDistSphereLoc : public Coefficient
{
private:
   ParGridFunction &dist;
   const real_t dx;

public:
   ExactDistSphereLoc(ParGridFunction &d)
      : dist(d), dx(dist.ParFESpace()->GetParMesh()->GetElementSize(0)) { }

   virtual real_t Eval(ElementTransformation &T, const IntegrationPoint &ip)
   {
      Vector pos(T.GetDimension());
      T.Transform(ip, pos);
      pos -= 0.5;
      const real_t r = sqrt(pos * pos);

      // One zone length in every direction.
      if (fabs(r - radius) < dx) { return fabs(r - radius); }
      else                       { return dist.GetValue(T, ip); }
   }
};


real_t Gyroid(const Vector &xx)
{
   const real_t period = 2.0 * M_PI;
   real_t x = xx[0]*period;
   real_t y = xx[1]*period;
   real_t z = (xx.Size()==3) ? xx[2]*period : 0.0;

   return std::sin(x)*std::cos(y) +
          std::sin(y)*std::cos(z) +
          std::sin(z)*std::cos(x);
}

real_t Sph(const mfem::Vector &xx)
{
   real_t R=0.4;
   mfem::Vector lvec(3);
   lvec=0.0;
   for (int i=0; i<xx.Size(); i++)
   {
      lvec[i]=xx[i];
   }

   return lvec[0]*lvec[0]+lvec[1]*lvec[1]+lvec[2]*lvec[2]-R*R;
}

void DGyroid(const mfem::Vector &xx, mfem::Vector &vals)
{
   vals.SetSize(xx.Size());
   vals=0.0;

   real_t pp=4*M_PI;

   mfem::Vector lvec(3);
   lvec=0.0;
   for (int i=0; i<xx.Size(); i++)
   {
      lvec[i]=xx[i]*pp;
   }

   vals[0]=cos(lvec[0])*cos(lvec[1])-sin(lvec[2])*sin(lvec[0]);
   vals[1]=-sin(lvec[0])*sin(lvec[1])+cos(lvec[1])*cos(lvec[2]);
   if (xx.Size()>2)
   {
      vals[2]=-sin(lvec[1])*sin(lvec[2])+cos(lvec[2])*cos(lvec[0]);
   }

   vals*=pp;
}

void GetElementEdgeLengths(const ParMesh & pmesh, int elem, Vector & length)
{
   Array<int> edges, cor, vert;
   pmesh.GetElementEdges(elem, edges, cor);

   length.SetSize(edges.Size());
   length = 0.0;

   for (int i=0; i<edges.Size(); ++i)
   {
      pmesh.GetEdgeVertices(edges[i], vert);

      const real_t *v0 = pmesh.GetVertex(vert[0]);
      const real_t *v1 = pmesh.GetVertex(vert[1]);

      const int spaceDim = pmesh.SpaceDimension();

      for (int k = 0; k < spaceDim; k++)
      {
         length[i] += (v0[k]-v1[k])*(v0[k]-v1[k]);
      }

      length[i] = sqrt(length[i]);
   }
}

real_t GetMinimumElementEdgeLength(const ParMesh & pmesh, int elem)
{
   Vector lengths;
   GetElementEdgeLengths(pmesh, elem, lengths);
   return lengths.Min();
}

real_t GetMaximumElementEdgeLength(const ParMesh & pmesh, int elem)
{
   Vector lengths;
   GetElementEdgeLengths(pmesh, elem, lengths);
   return lengths.Max();
}

void RefineUsingDistance(const ParGridFunction & dist_s,
                         const ParGridFunction & dist_v, int iter)
{
   ParMesh *pmesh = dist_v.ParFESpace()->GetParMesh();
   const int dim = pmesh->Dimension();

   constexpr bool use_scalar = true;

   constexpr real_t relTol = 0.1;
   constexpr real_t a = 0.25;  // Refinement scaling, <= 0.5

   real_t maxDist = 0.1;

   Array<Refinement> refs;  // Refinement is defined in ncmesh.hpp
   Array<real_t> refDist;

   for (int el=0; el<pmesh->GetNE(); ++el)
   {
      IntegrationPoint ip;
      std::vector<Vector> v(8);

      Vector center;
      pmesh->GetElementCenter(el, center);

      cout << el << ": center (" << center[0] << ", " << center[1] << ")" << endl;

      bool minInit = false;
      real_t elMinDist, elMaxDist;

      real_t elAvgDist = 0.0;

      int id = 0;
      for (int i=0; i<2; ++i)
      {
         const real_t xh = i;
         for (int j=0; j<2; ++j)
         {
            const real_t yh = j;

            ip.Set2(xh, yh);
            dist_v.GetVectorValue(el, ip, v[id]);

            const real_t vs = dist_s.GetValue(el, ip);

            cout << el << ": v at (" << i << ", " << j << ") = (" << v[id][0]
                 << ", " << v[id][1] << ")" << " nrm " << v[id].Norml2()
                 << ", scalar dist " << vs << endl;

            MFEM_ASSERT(minInit == (id > 0), ""); // TODO: eliminate minInit?

            const real_t dist = use_scalar ? vs : v[id].Norml2();

            if (!minInit)
            {
               elMinDist = dist;
               elMaxDist = elMinDist;
               minInit = true;
            }
            else
            {
               elMinDist = std::min(elMinDist, dist);
               elMaxDist = std::max(elMaxDist, dist);
            }

            elAvgDist += dist;

            id++;

            /*
            for (int k=0; k<2; ++k)
            {
              const real_t zh = k;

              ip.Set3(xh, yh, zh);
              dist_v.GetVectorValue(el, ip, v);

              cout << "v " << v.Norml2() << endl;
            }
                 */
         }
      }

      if (elMinDist > maxDist)
      {
         continue;   // Do not refine elements sufficiently far away.
      }

      elAvgDist /= (real_t) id;

      // Decide the type of refinement, based on distances.

      if (dim == 2)
      {
         const real_t iTol = 1.0e-6 * elMaxDist;
         const bool intersection = v[0] * v[3] < -iTol || v[1] * v[2] < -iTol;

         //const real_t minLength = GetMinimumElementEdgeLength(*pmesh, el);
         const real_t maxLength = GetMaximumElementEdgeLength(*pmesh, el);

         // Do not refine an element more than the element's diameter away from the zero set.
         if (elMinDist > 0.5*maxLength)
         {
            continue;
         }

         if (intersection)
         {
            // This element intersects the zero set. In this case, simply do
            // uniform isotropic refinement.
            refs.Append(Refinement(el, Refinement::XY));
            refDist.Append(elAvgDist);
         }
         else
         {
            // Find the vertex or edge closest to the zero set.
            std::set<int> closeVertices;
            for (int i=0; i<4; ++i)
            {
               const real_t dist_i = v[i].Norml2();
               const real_t relDist_i = (dist_i - elMinDist) / elMaxDist;
               if (relDist_i < relTol)
               {
                  closeVertices.insert(i);
               }
            }

            const int numCloseVertices = closeVertices.size();
            MFEM_VERIFY(numCloseVertices == 1 || numCloseVertices == 2, "");

            if (numCloseVertices == 1)
            {
               // Refine close to the vertex.
               const int idx = *closeVertices.begin();
               const int ix = idx / 2;
               const int iy = idx - (2 * ix);

               const real_t sx = ix == 0 ? a : 1.0 - a;
               const real_t sy = iy == 0 ? a : 1.0 - a;

               refs.Append(Refinement(el, {{Refinement::Type::X, sx},
                  {Refinement::Type::Y, sy}
               }));
               refDist.Append(elAvgDist);
            }
            else
            {
               // Refine close to the edge.
               int vij[2][2];
               int cnt = 0;
               for (auto idx : closeVertices)
               {
                  const int ix = idx / 2;
                  const int iy = idx - (2 * ix);

                  vij[cnt][0] = ix;
                  vij[cnt][1] = iy;
                  cnt++;
               }

               const bool xedge = vij[0][0] != vij[1][0];
               const auto type = xedge ? Refinement::Y : Refinement::X;

               real_t scale = 0.5;

               if (xedge)
               {
                  scale = vij[0][1] == 0 ? a : 1.0 - a;
               }
               else
               {
                  scale = vij[0][0] == 0 ? a : 1.0 - a;
               }

               refs.Append(Refinement(el, type, scale));
            }
         }
      }
      else
      {
         MFEM_ABORT("TODO");
      }
   }

   pmesh->GeneralRefinement(refs);

   pmesh->SetScaledNCMesh();

   ofstream mesh_ofs("refined" + std::to_string(iter) +".mesh");
   mesh_ofs.precision(8);
   pmesh->ParPrint(mesh_ofs);
}

int main(int argc, char *argv[])
{
   // Initialize MPI and HYPRE.
   Mpi::Init(argc, argv);
   int myid = Mpi::WorldRank();
   Hypre::Init();

   // Parse command-line options.
   const char *mesh_file = "../../data/inline-quad.mesh";
   int solver_type = 0;
   int problem = 1;
   int rs_levels = 2;
   int order = 2;
   int amr_iter = 0;
   real_t t_param = 1.0;
   const char *device_config = "cpu";
   int visport = 19916;
   bool visualization = true;

   OptionsParser args(argc, argv);
   args.AddOption(&mesh_file, "-m", "--mesh",
                  "Mesh file to use.");
   args.AddOption(&solver_type, "-s", "--solver",
                  "Solver type:\n\t"
                  "0: Heat\n\t"
                  "1: P-Laplacian\n\t"
                  "2: Rvachev scaling");
   args.AddOption(&problem, "-p", "--problem",
                  "Problem type:\n\t"
                  "0: Point source\n\t"
                  "1: Circle / sphere level set in 2D / 3D\n\t"
                  "2: 2D sine-looking level set\n\t"
                  "3: Gyroid level set in 2D or 3D\n\t"
                  "4: Combo of a doughnut and swiss cheese shapes in 3D.");
   args.AddOption(&rs_levels, "-rs", "--refine-serial",
                  "Number of times to refine the mesh uniformly in serial.");
   args.AddOption(&order, "-o", "--order",
                  "Finite element order (polynomial degree) or -1 for"
                  " isoparametric space.");
   args.AddOption(&amr_iter, "-amr", "--amr-iter",
                  "Number of adaptive mesh refinement iterations.");
   args.AddOption(&t_param, "-t", "--t-param",
                  "Diffusion time step (scaled internally scaled by dx*dx).");
   args.AddOption(&device_config, "-d", "--device",
                  "Device configuration string, see Device::Configure().");
   args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                  "--no-visualization",
                  "Enable or disable GLVis visualization.");
   args.AddOption(&visport, "-p", "--send-port", "Socket for GLVis.");
   args.Parse();
   if (!args.Good())
   {
      if (myid == 0)
      {
         args.PrintUsage(cout);
      }
      return 1;
   }
   if (myid == 0) { args.PrintOptions(cout); }

   // Enable hardware devices such as GPUs, and programming models such as CUDA,
   // OCCA, RAJA and OpenMP based on command line options.
   Device device(device_config);
   if (myid == 0) { device.Print(); }

   // Refine the mesh.
   Mesh mesh(mesh_file, 1, 1);
   const int dim = mesh.Dimension();
   for (int lev = 0; lev < rs_levels; lev++) { mesh.UniformRefinement(); }

   mesh.EnsureNCMesh();

   // MPI distribution.
   ParMesh pmesh(MPI_COMM_WORLD, mesh);
   mesh.Clear();

   Coefficient *ls_coeff = nullptr;
   int smooth_steps;
   if (problem == 0)
   {
      ls_coeff = new DeltaCoefficient(0.5, 0.5, 1000.0);
      smooth_steps = 0;
   }
   else if (problem == 1)
   {
      ls_coeff = new FunctionCoefficient(sphere_ls);
      smooth_steps = 0;
   }
   else if (problem == 2)
   {
      ls_coeff = new FunctionCoefficient(sine_ls);
      smooth_steps = 0;
   }
   else if (problem == 3)
   {
      ls_coeff = new FunctionCoefficient(Gyroid);
      smooth_steps = 0;
   }
   else if (problem == 4)
   {
      ls_coeff = new FunctionCoefficient(doughnut_cheese);
      smooth_steps = 0;
   }
   else { MFEM_ABORT("Unrecognized -problem option."); }

   const real_t dx = AvgElementSize(pmesh);
   DistanceSolver *dist_solver = NULL;
   if (solver_type == 0)
   {
      auto ds = new HeatDistanceSolver(t_param * dx * dx);
      if (problem == 0)
      {
         ds->transform = false;
      }
      ds->smooth_steps = smooth_steps;
      ds->vis_glvis = false;
      dist_solver = ds;
   }
   else if (solver_type == 1)
   {
      const int p = 10;
      const int newton_iter = 50;
      dist_solver = new PLapDistanceSolver(p, newton_iter);
   }
   else if (solver_type == 2)
   {
      dist_solver = new NormalizationDistanceSolver;
   }
   else { MFEM_ABORT("Wrong solver option."); }
   dist_solver->print_level.FirstAndLast().Summary();

   H1_FECollection fec(order, dim);
   ParFiniteElementSpace pfes_s(&pmesh, &fec), pfes_v(&pmesh, &fec, dim);
   ParGridFunction distance_s(&pfes_s), distance_v(&pfes_v);

   // Smooth-out Gibbs oscillations from the input level set. The smoothing
   // parameter here is specified to be mesh dependent with length scale dx.
   ParGridFunction filt_gf(&pfes_s);
   if (problem != 0)
   {
      real_t filter_weight = dx;
      // The normalization-based solver needs a more diffused input.
      if (solver_type == 2) { filter_weight *= 4.0; }
      PDEFilter filter(pmesh, filter_weight);
      filter.Filter(*ls_coeff, filt_gf);
   }
   else { filt_gf.ProjectCoefficient(*ls_coeff); }
   //delete ls_coeff;
   GridFunctionCoefficient ls_filt_coeff(&filt_gf);

   dist_solver->ComputeScalarDistance(ls_filt_coeff, distance_s);
   dist_solver->ComputeVectorDistance(ls_filt_coeff, distance_v);

   // Send the solution by socket to a GLVis server.
   if (visualization)
   {
      int size = 500;
      char vishost[] = "localhost";

      socketstream sol_sock_w;
      common::VisualizeField(sol_sock_w, vishost, visport, filt_gf,
                             "Input Level Set", 0, 0, size, size);

      MPI_Barrier(pmesh.GetComm());

      socketstream sol_sock_ds;
      common::VisualizeField(sol_sock_ds, vishost, visport, distance_s,
                             "Distance", size, 0, size, size,
                             "rRjmm********A");

      MPI_Barrier(pmesh.GetComm());

      socketstream sol_sock_dv;
      common::VisualizeField(sol_sock_dv, vishost, visport, distance_v,
                             "Directions", 2*size, 0, size, size,
                             "rRjmm********vveA");
   }

   // ParaView output.
   ParaViewDataCollection dacol("ParaViewDistance", &pmesh);
   dacol.SetLevelsOfDetail(order);
   dacol.RegisterField("filtered_level_set", &filt_gf);
   dacol.RegisterField("distance", &distance_s);
   dacol.SetTime(1.0);
   dacol.SetCycle(1);
   dacol.Save();

   // Save the mesh and the solution.
   ofstream mesh_ofs("distance.mesh");
   mesh_ofs.precision(8);
   pmesh.PrintAsOne(mesh_ofs);
   ofstream sol_ofs("distance.gf");
   sol_ofs.precision(8);
   distance_s.SaveAsOne(sol_ofs);

   ConstantCoefficient zero(0.0);
   const real_t s_norm  = distance_s.ComputeL2Error(zero),
                v_norm  = distance_v.ComputeL2Error(zero);
   if (myid == 0)
   {
      cout << fixed << setprecision(10) << "Norms: "
           << s_norm << " " << v_norm << endl;
   }

   if (problem == 1)
   {
      FunctionCoefficient exact_dist_coeff(exact_dist_sphere);
      const real_t error_l1 = distance_s.ComputeL1Error(exact_dist_coeff),
                   error_li = distance_s.ComputeMaxError(exact_dist_coeff);
      if (myid == 0)
      {
         cout << "Global L1 error:   " << error_l1 << endl
              << "Global Linf error: " << error_li << endl;
      }

      ExactDistSphereLoc exact_dist_coeff_loc(distance_s);
      const real_t error_l1_loc = distance_s.ComputeL1Error(exact_dist_coeff_loc),
                   error_li_loc = distance_s.ComputeMaxError(exact_dist_coeff_loc);
      if (myid == 0)
      {
         cout << "Local  L1 error:   " << error_l1_loc << endl
              << "Local  Linf error: " << error_li_loc << endl;
      }
   }

   for (int iter = 0; iter < amr_iter; ++iter)
   {
      RefineUsingDistance(distance_s, distance_v, iter);
      pfes_s.Update();
      pfes_v.Update();
      distance_s.Update();
      distance_v.Update();
      filt_gf.Update();

      {
         // Save the mesh and the solution.
         ofstream mesh_ofs_i("distance" + std::to_string(iter) + ".mesh");
         mesh_ofs_i.precision(8);
         pmesh.PrintAsOne(mesh_ofs_i);
         ofstream sol_ofs_i("distance" + std::to_string(iter) + ".gf");
         sol_ofs_i.precision(8);
         distance_v.SaveAsOne(sol_ofs_i);
      }

      if (problem == 0) { filt_gf.ProjectCoefficient(*ls_coeff); }

      //dist_solver->ComputeVectorDistance(ls_filt_coeff, distance_v);

      // TODO: solve again for distance on refined mesh? Or is interpolation after refinement
      // good enough?
   }

   delete dist_solver;

   return 0;
}
