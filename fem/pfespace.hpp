// Copyright (c) 2010-2025, Lawrence Livermore National Security, LLC. Produced
// at the Lawrence Livermore National Laboratory. All Rights reserved. See files
// LICENSE and NOTICE for details. LLNL-CODE-806117.
//
// This file is part of the MFEM library. For more information and source code
// availability visit https://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the BSD-3 license. We welcome feedback and contributions, see file
// CONTRIBUTING.md for details.

#ifndef MFEM_PFESPACE
#define MFEM_PFESPACE

#include "../config/config.hpp"

#ifdef MFEM_USE_MPI

#include "../linalg/hypre.hpp"
#include "../mesh/pmesh.hpp"
#include "../mesh/nurbs.hpp"
#include "fespace.hpp"

namespace mfem
{

/// Abstract parallel finite element space.
class ParFiniteElementSpace : public FiniteElementSpace
{
private:
   /// MPI data.
   MPI_Comm MyComm;
   int NRanks, MyRank;

   /// Parallel mesh; #mesh points to this object as well. Not owned.
   ParMesh *pmesh;
   /** Parallel non-conforming mesh extension object; same as pmesh->pncmesh.
       Not owned. */
   ParNCMesh *pncmesh;

   /// GroupCommunicator on the local VDofs. Owned.
   GroupCommunicator *gcomm;

   /// Number of true dofs in this processor (local true dofs).
   mutable int ltdof_size;

   /// Number of vertex/edge/face/total ghost DOFs (nonconforming case).
   int ngvdofs, ngedofs, ngfdofs, ngdofs;

   /// The group of each local dof.
   Array<int> ldof_group;

   /// For a local dof: the local true dof number in the master of its group.
   mutable Array<int> ldof_ltdof;

   /// Offsets for the dofs in each processor in global numbering.
   mutable Array<HYPRE_BigInt> dof_offsets;

   /// Offsets for the true dofs in each processor in global numbering.
   mutable Array<HYPRE_BigInt> tdof_offsets;

   /// Offsets for the true dofs in neighbor processor in global numbering.
   mutable Array<HYPRE_BigInt> tdof_nb_offsets;

   /// Previous 'dof_offsets' (before Update()), column partition of T.
   Array<HYPRE_BigInt> old_dof_offsets;

   /// The sign of the basis functions at the scalar local dofs.
   Array<int> ldof_sign;

   /// The matrix P (interpolation from true dof to dof). Owned.
   mutable HypreParMatrix *P;
   /// Optimized action-only prolongation operator for conforming meshes. Owned.
   mutable Operator *Pconf;

   /** Used to indicate that the space is nonconforming (even if the underlying
       mesh has NULL @a ncmesh). This occurs in low-order preconditioning on
       nonconforming meshes. */
   bool nonconf_P;

   /// The (block-diagonal) matrix R (restriction of dof to true dof). Owned.
   mutable SparseMatrix *R;
   /// Optimized action-only restriction operator for conforming meshes. Owned.
   mutable Operator *Rconf;

   /// Flag indicating the existence of shared triangles with interior ND dofs
   bool nd_strias;

   /// Resets nd_strias flag at construction or after rebalancing
   void CheckNDSTriaDofs();

   ParNURBSExtension *pNURBSext() const
   { return dynamic_cast<ParNURBSExtension *>(NURBSext); }

   static ParNURBSExtension *MakeLocalNURBSext(
      const NURBSExtension *globNURBSext, const NURBSExtension *parNURBSext);

   GroupTopology &GetGroupTopo() const
   { return (NURBSext) ? pNURBSext()->gtopo : pmesh->gtopo; }

   // Auxiliary method used in constructors
   void ParInit(ParMesh *pm);

   void Construct();
   void Destroy();

   // ldof_type = 0 : DOFs communicator, otherwise VDOFs communicator
   void GetGroupComm(GroupCommunicator &gcomm, int ldof_type,
                     Array<int> *ldof_sign = NULL);

   /// Construct dof_offsets and tdof_offsets using global communication.
   void GenerateGlobalOffsets() const;

   /// Construct ldof_group and ldof_ltdof.
   void ConstructTrueDofs();
   void ConstructTrueNURBSDofs();

   void ApplyLDofSigns(Array<int> &dofs) const;
   void ApplyLDofSigns(Table &el_dof) const;

   typedef NCMesh::MeshId MeshId;
   typedef ParNCMesh::GroupId GroupId;

   void GetGhostVertexDofs(const MeshId &id, Array<int> &dofs) const;
   void GetGhostEdgeDofs(const MeshId &edge_id, Array<int> &dofs) const;
   void GetGhostFaceDofs(const MeshId &face_id, Array<int> &dofs) const;
   void GetGhostDofs(int entity, const MeshId &id, Array<int> &dofs) const;

   /// Return the dofs associated with the interior of the given mesh entity.
   void GetBareDofs(int entity, int index, Array<int> &dofs) const;

   int  PackDof(int entity, int index, int edof) const;
   void UnpackDof(int dof, int &entity, int &index, int &edof) const;

#ifdef MFEM_PMATRIX_STATS
   mutable int n_msgs_sent, n_msgs_recv;
   mutable int n_rows_sent, n_rows_recv, n_rows_fwd;
#endif

   void ScheduleSendRow(const struct PMatrixRow &row, int dof, GroupId group_id,
                        std::map<int, class NeighborRowMessage> &send_msg) const;

   void ForwardRow(const struct PMatrixRow &row, int dof,
                   GroupId group_sent_id, GroupId group_id,
                   std::map<int, class NeighborRowMessage> &send_msg) const;

#ifdef MFEM_DEBUG_PMATRIX
   void DebugDumpDOFs(std::ostream &os,
                      const SparseMatrix &deps,
                      const Array<GroupId> &dof_group,
                      const Array<GroupId> &dof_owner,
                      const Array<bool> &finalized) const;
#endif

   /// Helper: create a HypreParMatrix from a list of PMatrixRows.
   HypreParMatrix*
   MakeVDimHypreMatrix(const std::vector<struct PMatrixRow> &rows,
                       int local_rows, int local_cols,
                       Array<HYPRE_BigInt> &row_starts,
                       Array<HYPRE_BigInt> &col_starts) const;

   /// Build the P and R matrices.
   void Build_Dof_TrueDof_Matrix() const;

   /** Used when the ParMesh is non-conforming, i.e. pmesh->pncmesh != NULL.
       Constructs the matrices P and R, the DOF and true DOF offset arrays,
       and the DOF -> true DOF map ('dof_tdof'). Returns the number of
       vector true DOFs. All pointer arguments are optional and can be NULL. */
   int BuildParallelConformingInterpolation(HypreParMatrix **P, SparseMatrix **R,
                                            Array<HYPRE_BigInt> &dof_offs,
                                            Array<HYPRE_BigInt> &tdof_offs,
                                            Array<int> *dof_tdof,
                                            bool partial = false) const;

   /** Calculate a GridFunction migration matrix after mesh load balancing.
       The result is a parallel permutation matrix that can be used to update
       all grid functions defined on this space. */
   HypreParMatrix* RebalanceMatrix(int old_ndofs,
                                   const Table* old_elem_dof,
                                   const Table* old_elem_fos);

   /** Calculate a GridFunction restriction matrix after mesh derefinement.
       The matrix is constructed so that the new grid function interpolates
       the original function, i.e., the original function is evaluated at the
       nodes of the coarse function. */
   HypreParMatrix* ParallelDerefinementMatrix(int old_ndofs,
                                              const Table *old_elem_dof,
                                              const Table *old_elem_fos);

   /// Updates the internal mesh pointer. @warning @a new_mesh must be
   /// <b>topologically identical</b> to the existing mesh. Used if the address
   /// of the Mesh object has changed, e.g. in @a Mesh::Swap.
   void UpdateMeshPointer(Mesh *new_mesh) override;

   /// Copies the prolongation and restriction matrices from @a fes.
   ///
   /// Used for low order preconditioning on non-conforming meshes. If the DOFs
   /// require a permutation, it will be supplied by non-NULL @a perm. NULL @a
   /// perm indicates that no permutation is required.
   void CopyProlongationAndRestriction(const FiniteElementSpace &fes,
                                       const Array<int> *perm) override;

public:
   // Face-neighbor data
   // Number of face-neighbor dofs
   int num_face_nbr_dofs;
   // Face-neighbor-element to face-neighbor dof
   Table face_nbr_element_dof;
   // Face-neighbor-element face orientations
   Table face_nbr_element_fos;
   // Face-neighbor to ldof in the face-neighbor numbering
   Table face_nbr_ldof;
   // The global ldof indices of the face-neighbor dofs
   Array<HYPRE_BigInt> face_nbr_glob_dof_map;
   // Local face-neighbor data: face-neighbor to ldof
   Table send_face_nbr_ldof;

   /** @brief Copy constructor: deep copy all data from @a orig except the
       ParMesh, the FiniteElementCollection, and some derived data. */
   /** If the @a pmesh or @a fec pointers are NULL (default), then the new
       ParFiniteElementSpace will reuse the respective pointers from @a orig. If
       any of these pointers is not NULL, the given pointer will be used instead
       of the one used by @a orig.

       @note The objects pointed to by the @a pmesh and @a fec parameters must
       be either the same objects as the ones used by @a orig, or copies of
       them. Otherwise, the behavior is undefined.

       @note Derived data objects, such as the parallel prolongation and
       restriction operators, the update operator, and any of the face-neighbor
       data, will not be copied, even if they are created in the @a orig object.
   */
   ParFiniteElementSpace(const ParFiniteElementSpace &orig,
                         ParMesh *pmesh = NULL,
                         const FiniteElementCollection *fec = NULL);

   /** @brief Convert/copy the *local* (Par)FiniteElementSpace @a orig to
       ParFiniteElementSpace: deep copy all data from @a orig except the Mesh,
       the FiniteElementCollection, and some derived data. */
   ParFiniteElementSpace(const FiniteElementSpace &orig, ParMesh &pmesh,
                         const FiniteElementCollection *fec = NULL);

   /** @brief Construct the *local* ParFiniteElementSpace corresponding to the
       global FE space, @a global_fes. */
   /** The parameter @a pm is the *local* ParMesh obtained by decomposing the
       global Mesh used by @a global_fes. The array @a partitioning represents
       the parallel decomposition - it maps global element ids to MPI ranks.
       If the FiniteElementCollection, @a f, is NULL (default), the FE
       collection used by @a global_fes will be reused. If @a f is not NULL, it
       must be the same as, or a copy of, the FE collection used by
       @a global_fes.

       @note Currently the @a partitioning array is not used by this
       constructor, it is required for general parallel variable-order support.
   */
   ParFiniteElementSpace(ParMesh *pm, const FiniteElementSpace *global_fes,
                         const int *partitioning,
                         const FiniteElementCollection *f = NULL);

   ParFiniteElementSpace(ParMesh *pm, const FiniteElementCollection *f,
                         int dim = 1, int ordering = Ordering::byNODES);

   /// Construct a NURBS FE space based on the given NURBSExtension, @a ext.
   /** The parameter @a ext will be deleted by this constructor, replaced by a
       ParNURBSExtension owned by the ParFiniteElementSpace.
       @note If the pointer @a ext is NULL, this constructor is equivalent to
       the standard constructor with the same arguments minus the
       NURBSExtension, @a ext. */
   ParFiniteElementSpace(ParMesh *pm, NURBSExtension *ext,
                         const FiniteElementCollection *f,
                         int dim = 1, int ordering = Ordering::byNODES);

   MPI_Comm GetComm() const { return MyComm; }
   int GetNRanks() const { return NRanks; }
   int GetMyRank() const { return MyRank; }

   inline ParMesh *GetParMesh() const { return pmesh; }

   int GetDofSign(int i)
   { return NURBSext || Nonconforming() ? 1 : ldof_sign[VDofToDof(i)]; }
   HYPRE_BigInt *GetDofOffsets()     const { return dof_offsets; }
   HYPRE_BigInt *GetTrueDofOffsets() const { return tdof_offsets; }
   HYPRE_BigInt GlobalVSize() const
   { return Dof_TrueDof_Matrix()->GetGlobalNumRows(); }
   HYPRE_BigInt GlobalTrueVSize() const
   { return Dof_TrueDof_Matrix()->GetGlobalNumCols(); }

   /// Return the number of local vector true dofs.
   int GetTrueVSize() const override { return ltdof_size; }

   /// Returns indexes of degrees of freedom in array dofs for i'th element and
   /// returns the DofTransformation data in a user-provided object.
   using FiniteElementSpace::GetElementDofs;
   void GetElementDofs(int i, Array<int> &dofs,
                       DofTransformation &doftrans) const override;

   /// Returns indexes of degrees of freedom for i'th boundary element and
   /// returns the DofTransformation data in a user-provided object.
   using FiniteElementSpace::GetBdrElementDofs;
   void GetBdrElementDofs(int i, Array<int> &dofs,
                          DofTransformation &doftrans) const override;

   /** Returns the indexes of the degrees of freedom for i'th face
       including the dofs for the edges and the vertices of the face. */
   int GetFaceDofs(int i, Array<int> &dofs, int variant = 0) const override;

   /** Returns pointer to the FiniteElement in the FiniteElementCollection
       associated with i'th element in the mesh object. If @a i is greater than
       or equal to the number of local mesh elements, @a i will be interpreted
       as a shifted index of a face neighbor element. */
   const FiniteElement *GetFE(int i) const override;

   /** Returns an Operator that converts L-vectors to E-vectors on each face.
       The parallel version is different from the serial one because of the
       presence of shared faces. Shared faces are treated as interior faces,
       the returned operator handles the communication needed to get the
       shared face values from other MPI ranks */
   const FaceRestriction *GetFaceRestriction(
      ElementDofOrdering f_ordering, FaceType type,
      L2FaceValues mul = L2FaceValues::DoubleValued) const override;

   void GetSharedEdgeDofs(int group, int ei, Array<int> &dofs) const;
   void GetSharedTriangleDofs(int group, int fi, Array<int> &dofs) const;
   void GetSharedQuadrilateralDofs(int group, int fi, Array<int> &dofs) const;

   /// The true dof-to-dof interpolation matrix
   HypreParMatrix *Dof_TrueDof_Matrix() const
   { if (!P) { Build_Dof_TrueDof_Matrix(); } return P; }

   /** @brief For a non-conforming mesh, construct and return the interpolation
       matrix from the partially conforming true dofs to the local dofs. */
   /** @note The returned pointer must be deleted by the caller. */
   HypreParMatrix *GetPartialConformingInterpolation();

   /** Create and return a new HypreParVector on the true dofs, which is
       owned by (i.e. it must be destroyed by) the calling function. */
   HypreParVector *NewTrueDofVector()
   { return (new HypreParVector(MyComm,GlobalTrueVSize(),GetTrueDofOffsets()));}

   /// Scale a vector of true dofs
   void DivideByGroupSize(real_t *vec);

   /// Return a reference to the internal GroupCommunicator (on VDofs)
   GroupCommunicator &GroupComm() { return *gcomm; }

   /// Return a const reference to the internal GroupCommunicator (on VDofs)
   const GroupCommunicator &GroupComm() const { return *gcomm; }

   /// Return a new GroupCommunicator on scalar dofs, i.e. for VDim = 1.
   /** @note The returned pointer must be deleted by the caller. */
   GroupCommunicator *ScalarGroupComm();

   /** @brief Given an integer array on the local degrees of freedom, perform
       a bitwise OR between the shared dofs. */
   /** For non-conforming mesh, synchronization is performed on the cut (aka
       "partially conforming") space. */
   void Synchronize(Array<int> &ldof_marker) const;

   /// Determine the boundary degrees of freedom
   void GetEssentialVDofs(const Array<int> &bdr_attr_is_ess,
                          Array<int> &ess_dofs,
                          int component = -1) const override;

   /** Get a list of essential true dofs, ess_tdof_list, corresponding to the
       boundary attributes marked in the array bdr_attr_is_ess. */
   void GetEssentialTrueDofs(const Array<int> &bdr_attr_is_ess,
                             Array<int> &ess_tdof_list,
                             int component = -1) const override;

   /// Determine the external degrees of freedom
   void GetExteriorVDofs(Array<int> &ext_dofs,
                         int component = -1) const override;

   /** Get a list of external true dofs, ext_tdof_list, corresponding to the
       face on the exterior of the mesh. */
   void GetExteriorTrueDofs(Array<int> &ext_tdof_list,
                            int component = -1) const override;

   /** If the given ldof is owned by the current processor, return its local
       tdof number, otherwise return -1 */
   int GetLocalTDofNumber(int ldof) const;
   /// Returns the global tdof number of the given local degree of freedom
   HYPRE_BigInt GetGlobalTDofNumber(int ldof) const;
   /** Returns the global tdof number of the given local degree of freedom in
       the scalar version of the current finite element space. The input should
       be a scalar local dof. */
   HYPRE_BigInt GetGlobalScalarTDofNumber(int sldof);

   HYPRE_BigInt GetMyDofOffset() const;
   HYPRE_BigInt GetMyTDofOffset() const;

   const Operator *GetProlongationMatrix() const override;
   /** Get an Operator that performs the action of GetRestrictionMatrix(),
       but potentially with a non-assembled optimized matrix-free
       implementation. */
   const Operator *GetRestrictionOperator() const override;
   /// Get the R matrix which restricts a local dof vector to true dof vector.
   const SparseMatrix *GetRestrictionMatrix() const override
   { Dof_TrueDof_Matrix(); return R; }

   // Face-neighbor functions
   void ExchangeFaceNbrData();
   int GetFaceNbrVSize() const { return num_face_nbr_dofs; }
   void GetFaceNbrElementVDofs(int i, Array<int> &vdofs,
                               DofTransformation &doftrans) const;
   DofTransformation *GetFaceNbrElementVDofs(int i, Array<int> &vdofs) const;
   void GetFaceNbrFaceVDofs(int i, Array<int> &vdofs) const;
   const FiniteElement *GetFaceNbrFE(int i) const;
   const FiniteElement *GetFaceNbrFaceFE(int i) const;
   const HYPRE_BigInt *GetFaceNbrGlobalDofMap() { return face_nbr_glob_dof_map; }
   ElementTransformation *GetFaceNbrElementTransformation(int i) const
   { return pmesh->GetFaceNbrElementTransformation(i); }

   void Lose_Dof_TrueDof_Matrix();
   void LoseDofOffsets() { dof_offsets.LoseData(); }
   void LoseTrueDofOffsets() { tdof_offsets.LoseData(); }

   bool Conforming() const { return pmesh->pncmesh == NULL && !nonconf_P; }
   bool Nonconforming() const { return pmesh->pncmesh != NULL || nonconf_P; }

   bool SharedNDTriangleDofs() const { return nd_strias; }

   // Transfer parallel true-dof data from coarse_fes, defined on a coarse mesh,
   // to this FE space, defined on a refined mesh. See full documentation in the
   // base class, FiniteElementSpace::GetTrueTransferOperator.
   void GetTrueTransferOperator(const FiniteElementSpace &coarse_fes,
                                OperatorHandle &T) const override;

   /** Reflect changes in the mesh. Calculate one of the refinement/derefinement
       /rebalance matrices, unless want_transform is false. */
   void Update(bool want_transform = true) override;

   /// Free ParGridFunction transformation matrix (if any), to save memory.
   void UpdatesFinished() override
   {
      FiniteElementSpace::UpdatesFinished();
      old_dof_offsets.DeleteAll();
   }

   virtual ~ParFiniteElementSpace() { Destroy(); }

   void PrintPartitionStats();

   /// Obsolete, kept for backward compatibility
   int TrueVSize() const { return ltdof_size; }
};


/// Auxiliary class used by ParFiniteElementSpace.
class ConformingProlongationOperator : public Operator
{
protected:
   Array<int> external_ldofs;
   const GroupCommunicator &gc;
   bool local;

public:
   ConformingProlongationOperator(int lsize, const GroupCommunicator &gc_,
                                  bool local_=false);

   ConformingProlongationOperator(const ParFiniteElementSpace &pfes,
                                  bool local_=false);

   const GroupCommunicator &GetGroupCommunicator() const;

   void Mult(const Vector &x, Vector &y) const override;

   void MultTranspose(const Vector &x, Vector &y) const override;
};

/// Auxiliary device class used by ParFiniteElementSpace.
class DeviceConformingProlongationOperator: public
   ConformingProlongationOperator
{
protected:
   bool mpi_gpu_aware;
   Array<int> shr_ltdof, ext_ldof;
   mutable Vector shr_buf, ext_buf;
   Memory<int> shr_buf_offsets, ext_buf_offsets;
   Array<int> ltdof_ldof, unq_ltdof;
   Array<int> unq_shr_i, unq_shr_j;
   MPI_Request *requests;

   // Kernel: copy ltdofs from 'src' to 'shr_buf' - prepare for send.
   //         shr_buf[i] = src[shr_ltdof[i]]
   void BcastBeginCopy(const Vector &src) const;

   // Kernel: copy ltdofs from 'src' to ldofs in 'dst'.
   //         dst[ltdof_ldof[i]] = src[i]
   void BcastLocalCopy(const Vector &src, Vector &dst) const;

   // Kernel: copy ext. dofs from 'ext_buf' to 'dst' - after recv.
   //         dst[ext_ldof[i]] = ext_buf[i]
   void BcastEndCopy(Vector &dst) const;

   // Kernel: copy ext. dofs from 'src' to 'ext_buf' - prepare for send.
   //         ext_buf[i] = src[ext_ldof[i]]
   void ReduceBeginCopy(const Vector &src) const;

   // Kernel: copy owned ldofs from 'src' to ltdofs in 'dst'.
   //         dst[i] = src[ltdof_ldof[i]]
   void ReduceLocalCopy(const Vector &src, Vector &dst) const;

   // Kernel: assemble dofs from 'shr_buf' into to 'dst' - after recv.
   //         dst[shr_ltdof[i]] += shr_buf[i]
   void ReduceEndAssemble(Vector &dst) const;

public:
   DeviceConformingProlongationOperator(
      const GroupCommunicator &gc_, const SparseMatrix *R, bool local_=false);

   DeviceConformingProlongationOperator(const ParFiniteElementSpace &pfes,
                                        bool local_=false);

   virtual ~DeviceConformingProlongationOperator();

   void Mult(const Vector &x, Vector &y) const override;

   void MultTranspose(const Vector &x, Vector &y) const override;
};

}

#endif // MFEM_USE_MPI

#endif
