#pragma once

#include <color_spinor_field.h>
#include <tune_quda.h>
#include <dslash_quda.h>
#include <dslash_helper.cuh>
#include <jitify_helper.cuh>

namespace quda {

  template <typename Float>
  class Dslash : public TunableVectorYZ {

  protected:

    DslashArg<Float> &arg;
    const ColorSpinorField &out;
    const ColorSpinorField &in;

    const int nDimComms;

    char aux_base[TuneKey::aux_n];
    char aux[8][TuneKey::aux_n];

    char aux_pack[TuneKey::aux_n];

    // pointers to ghost buffers we are packing to
    void *packBuffer[2 * QUDA_MAX_DIM];

#ifdef JITIFY
    // local copy of the static program pointer - this is a work
    // around for issues with the static program pointer when
    // HOSTDEBUG compilation is targeted (more precisely -fno-inline)
    jitify::Program *program_;
#endif

    /**
       @brief Set the base strings used by the different dslash kernel
       types for autotuning.
    */
    inline void fillAuxBase() {
      char comm[5];
      comm[0] = (arg.commDim[0] ? '1' : '0');
      comm[1] = (arg.commDim[1] ? '1' : '0');
      comm[2] = (arg.commDim[2] ? '1' : '0');
      comm[3] = (arg.commDim[3] ? '1' : '0');
      comm[4] = '\0';
      strcpy(aux_base,",commDim=");
      strcat(aux_base,comm);

      if (arg.xpay) strcat(aux_base,",xpay");
      if (arg.dagger) strcat(aux_base,",dagger");
    }

    /**
       @brief Specialize the auxiliary strings for each kernel type
       @param[in] kernel_type The kernel_type we are generating the string got
       @param[in] kernel_str String corresponding to the kernel type
    */
    inline void fillAux(KernelType kernel_type, const char *kernel_str) {
      strcpy(aux[kernel_type],kernel_str);
      if (kernel_type == INTERIOR_KERNEL) strcat(aux[kernel_type],comm_dim_partitioned_string());
      strcat(aux[kernel_type],aux_base);
    }

    bool tuneGridDim() const { return false; }
    unsigned int minThreads() const { return arg.threads; }

    template <typename Arg>
    inline void setParam(Arg &arg) {
      arg.t_proj_scale = getKernelPackT() ? 1.0 : 2.0;

      // Need to reset ghost pointers prior to every call since the
      // ghost buffer may have been changed during policy tuning.
      // Also, the accessor constructor calls Ghost(), which uses
      // ghost_buf, but this is only presently set with the
      // synchronous exchangeGhost.
      static void *ghost[8]; // needs to be persistent across interior and exterior calls
      for (int dim=0; dim<4; dim++) {

        for (int dir=0; dir<2; dir++) {
          // if doing interior kernel, then this is the initial call,
          // so we set all ghost pointers else if doing exterior
          // kernel, then we only have to update the non-p2p ghosts,
          // since these may have been assigned to zero-copy memory
          if (!comm_peer2peer_enabled(dir, dim) || arg.kernel_type == INTERIOR_KERNEL) {
            ghost[2*dim+dir] = (Float*)((char*)in.Ghost2() + in.GhostOffset(dim,dir)*in.GhostPrecision());
          }
        }
      }

      arg.in.resetGhost(in, ghost);
    }

    virtual int tuningIter() const { return 10; }

    int blockStep() const { return 16; }
    int blockMin() const { return 16; }

    unsigned int maxSharedBytesPerBlock() const {
      return maxDynamicSharedBytesPerBlock();
    }

    bool advanceAux(TuneParam &param) const {
      if (arg.pack_threads && arg.kernel_type == INTERIOR_KERNEL) {
        // if doing the fused kernel we tune how many blocks to use for
        // communication
        constexpr int max_blocks_per_dir = 4;
        if (param.aux.x + 1 <= max_blocks_per_dir) {
          param.aux.x++;
          return true;
        } else {
          param.aux.x = 1;
          return false;
        }
      } else {
        return false;
      }
    }

    void initTuneParam(TuneParam &param) const {
      TunableVectorYZ::initTuneParam(param);
      if (arg.pack_threads && arg.kernel_type == INTERIOR_KERNEL)
        param.aux.x = 1; // packing blocks per direction
    }

    void defaultTuneParam(TuneParam &param) const {
      TunableVectorYZ::defaultTuneParam(param);
      if (arg.pack_threads && arg.kernel_type == INTERIOR_KERNEL)
        param.aux.x = 1; // packing blocks per direction
    }

  public:

    template <typename T, typename Arg>
    inline void launch(T *f, const TuneParam &tp, Arg &arg, const cudaStream_t &stream) {
      if (deviceProp.major >= 7) { // should test whether this is always optimal on Volta
	this->setMaxDynamicSharedBytesPerBlock(f);
      }
      void *args[] = { &arg };
      qudaLaunchKernel((const void *)f, tp.grid, tp.block, args, tp.shared_bytes, stream);
    }

    /**
       @brief This instantiate function is used to instantiate the
       the KernelType template required for the multi-GPU dslash kernels.
       @param[in] tp The tuning parameters to use for this kernel
       @param[in,out] arg The argument struct for the kernel
       @param[in] stream The cudaStream_t where the kernel will run
     */
    template < template <typename,int,int,int,bool,bool,KernelType,typename> class Launch,
               int nDim, int nColor, int nParity, bool dagger, bool xpay, typename Arg>
    inline void instantiate(TuneParam &tp, Arg &arg, const cudaStream_t &stream) {

      if (in.Location() == QUDA_CPU_FIELD_LOCATION) {
        errorQuda("Not implemented");
      } else {
        switch(arg.kernel_type) {
        case INTERIOR_KERNEL:
          Launch<Float,nDim,nColor,nParity,dagger,xpay,INTERIOR_KERNEL,Arg>::launch(*this, tp, arg, stream); break;
#ifdef MULTI_GPU
        case EXTERIOR_KERNEL_X:
          Launch<Float,nDim,nColor,nParity,dagger,xpay,EXTERIOR_KERNEL_X,Arg>::launch(*this, tp, arg, stream); break;
        case EXTERIOR_KERNEL_Y:
          Launch<Float,nDim,nColor,nParity,dagger,xpay,EXTERIOR_KERNEL_Y,Arg>::launch(*this, tp, arg, stream); break;
        case EXTERIOR_KERNEL_Z:
          Launch<Float,nDim,nColor,nParity,dagger,xpay,EXTERIOR_KERNEL_Z,Arg>::launch(*this, tp, arg, stream); break;
        case EXTERIOR_KERNEL_T:
          Launch<Float,nDim,nColor,nParity,dagger,xpay,EXTERIOR_KERNEL_T,Arg>::launch(*this, tp, arg, stream); break;
        case EXTERIOR_KERNEL_ALL:
          Launch<Float,nDim,nColor,nParity,dagger,xpay,EXTERIOR_KERNEL_ALL,Arg>::launch(*this, tp, arg, stream); break;
        default: errorQuda("Unexpected kernel type %d", arg.kernel_type);
#else
        default: errorQuda("Unexpected kernel type %d for single-GPU build", arg.kernel_type);
#endif
        }
      }
    }

    /**
       @brief This instantiate function is used to instantiate the
       the dagger template
       @param[in] tp The tuning parameters to use for this kernel
       @param[in,out] arg The argument struct for the kernel
       @param[in] stream The cudaStream_t where the kernel will run
     */
    template < template <typename,int,int,int,bool,bool,KernelType,typename> class Launch,
               int nDim, int nColor, int nParity, bool xpay, typename Arg>
    inline void instantiate(TuneParam &tp, Arg &arg, const cudaStream_t &stream)
    {
#ifdef JITIFY
      using namespace jitify::reflection;
      const auto kernel = Launch<void,0,0,0,false,false,INTERIOR_KERNEL,Arg>::kernel;
      Tunable::jitify_error = program_->kernel(kernel)
        .instantiate(Type<Float>(),nDim,nColor,nParity,arg.dagger,xpay,arg.kernel_type,Type<Arg>())
        .configure(tp.grid,tp.block,tp.shared_bytes,stream)
        .launch(arg);
#else
      if (arg.dagger) instantiate<Launch,nDim,nColor,nParity, true,xpay>(tp, arg, stream);
      else            instantiate<Launch,nDim,nColor,nParity,false,xpay>(tp, arg, stream);
#endif
    }

    /**
       @brief This instantiate function is used to instantiate the
       the nParity template
       @param[in] tp The tuning parameters to use for this kernel
       @param[in,out] arg The argument struct for the kernel
       @param[in] stream The cudaStream_t where the kernel will run
     */
    template < template <typename,int,int,int,bool,bool,KernelType,typename> class Launch,
               int nDim, int nColor, bool xpay, typename Arg>
    inline void instantiate(TuneParam &tp, Arg &arg, const cudaStream_t &stream)
    {
#ifdef JITIFY
      using namespace jitify::reflection;
      const auto kernel = Launch<void,0,0,0,false,false,INTERIOR_KERNEL,Arg>::kernel;
      Tunable::jitify_error = program_->kernel(kernel)
        .instantiate(Type<Float>(),nDim,nColor,arg.nParity,arg.dagger,xpay,arg.kernel_type,Type<Arg>())
        .configure(tp.grid,tp.block,tp.shared_bytes,stream)
        .launch(arg);
#else
      switch (arg.nParity) {
      case 1: instantiate<Launch,nDim,nColor,1,xpay>(tp, arg, stream); break;
      case 2: instantiate<Launch,nDim,nColor,2,xpay>(tp, arg, stream); break;
      default: errorQuda("nParity = %d undefined\n", arg.nParity);
      }
#endif
    }

    /**
       @brief This instantiate function is used to instantiate the
       the xpay template
       @param[in] tp The tuning parameters to use for this kernel
       @param[in,out] arg The argument struct for the kernel
       @param[in] stream The cudaStream_t where the kernel will run
     */
    template < template <typename,int,int,int,bool,bool,KernelType,typename> class Launch,
      int nDim, int nColor, typename Arg>
    inline void instantiate(TuneParam &tp, Arg &arg, const cudaStream_t &stream)
    {
#ifdef JITIFY
      using namespace jitify::reflection;
      const auto kernel = Launch<void,0,0,0,false,false,INTERIOR_KERNEL,Arg>::kernel;
      Tunable::jitify_error = program_->kernel(kernel)
        .instantiate(Type<Float>(),nDim,nColor,arg.nParity,arg.dagger,arg.xpay,arg.kernel_type,Type<Arg>())
        .configure(tp.grid,tp.block,tp.shared_bytes,stream)
        .launch(arg);
#else
      if (arg.xpay) instantiate<Launch,nDim,nColor, true>(tp, arg, stream);
      else          instantiate<Launch,nDim,nColor,false>(tp, arg, stream);
#endif
    }

    DslashArg<Float> &dslashParam; // temporary addition for policy compatibility

    Dslash(DslashArg<Float> &arg, const ColorSpinorField &out, const ColorSpinorField &in, const char *src)
      : TunableVectorYZ(1,arg.nParity), arg(arg), out(out), in(in), nDimComms(4), dslashParam(arg)
    {
      // this sets the communications pattern for the packing kernel
      setPackComms(arg.commDim);

      //strcpy(aux, in.AuxString());
      fillAuxBase();
#ifdef MULTI_GPU
      fillAux(INTERIOR_KERNEL, "policy_kernel=interior");
      fillAux(EXTERIOR_KERNEL_ALL, "policy_kernel=exterior_all");
      fillAux(EXTERIOR_KERNEL_X, "policy_kernel=exterior_x");
      fillAux(EXTERIOR_KERNEL_Y, "policy_kernel=exterior_y");
      fillAux(EXTERIOR_KERNEL_Z, "policy_kernel=exterior_z");
      fillAux(EXTERIOR_KERNEL_T, "policy_kernel=exterior_t");
#else
      fillAux(INTERIOR_KERNEL, "policy_kernel=single-GPU");
#endif // MULTI_GPU
      fillAux(KERNEL_POLICY, "policy");

#ifdef JITIFY
      create_jitify_program(src);
      program_ = program;
#endif
    }

    void setPack(bool pack, MemoryLocation location) {
      arg.setPack(pack);
      if (!pack)
        return;

      for (int dim = 0; dim < 4; dim++) {
        for (int dir = 0; dir < 2; dir++) {
          if ((location & Remote) &&
              comm_peer2peer_enabled(dir, dim)) { // pack to p2p remote
            packBuffer[2 * dim + dir] =
                static_cast<char *>(
                    in.ghost_remote_send_buffer_d[in.bufferIndex][dim][dir]) +
                in.Precision() * in.GhostOffset(dim, 1 - dir);
          } else if (location & Host &&
                     !comm_peer2peer_enabled(dir, dim)) { // pack to cpu memory
            packBuffer[2 * dim + dir] =
                in.my_face_dim_dir_hd[in.bufferIndex][dim][dir];
          } else { // pack to local gpu memory
            packBuffer[2 * dim + dir] =
                in.my_face_dim_dir_d[in.bufferIndex][dim][dir];
          }
        }
      }

      // set the tuning string for the fused interior + packer kernel
      strcpy(aux_pack, Dslash<Float>::aux[arg.kernel_type]);
      strcat(aux_pack, ",fused_pack");

      // label the locations we are packing to
      // location label is nonp2p-p2p
      switch ((int)location) {
      case Device | Remote:
        strcat(aux_pack, ",device-remote");
        break;
      case Host | Remote:
        strcat(aux_pack, ",host-remote");
        break;
      case Device:
        strcat(aux_pack, ",device-device");
        break;
      case Host:
        strcat(aux_pack,
               comm_peer2peer_enabled_global() ? ",host-device" : ",host-host");
        break;
      default:
        errorQuda("Unknown pack target location %d\n", location);
      }
    }

    int Nface() const { return 2*arg.nFace; } // factor of 2 is for forwards/backwards (convention used in dslash policy)
    int Dagger() const { return arg.dagger; }

    const char* getAux(KernelType type) const {
      return aux[type];
    }

    void setAux(KernelType type, const char *aux_) {
      strcpy(aux[type], aux_);
    }

    void augmentAux(KernelType type, const char *extra) {
      strcat(aux[type], extra);
    }

    /**
       @brief Save the output field since the output field is both
       read from and written to in the exterior kernels
     */
    virtual void preTune() { if (arg.kernel_type != INTERIOR_KERNEL && arg.kernel_type != KERNEL_POLICY) out.backup(); }

    /**
       @brief Restore the output field if doing exterior kernel
     */
    virtual void postTune() { if (arg.kernel_type != INTERIOR_KERNEL && arg.kernel_type != KERNEL_POLICY) out.restore(); }

    /*
      per direction / dimension flops
      spin project flops = Nc * Ns
      SU(3) matrix-vector flops = (8 Nc - 2) * Nc
      spin reconstruction flops = 2 * Nc * Ns (just an accumulation to all
      components) xpay = 2 * 2 * Nc * Ns

      So for the full dslash we have, where for the final spin
      reconstruct we have -1 since the first direction does not
      require any accumulation.

      flops = (2 * Nd * Nc * Ns)  +  (2 * Nd * (Ns/2) * (8*Nc-2) * Nc)  +  ((2 *
      Nd - 1) * 2 * Nc * Ns) flops_xpay = flops + 2 * 2 * Nc * Ns

      For Wilson this should give 1344 for Nc=3,Ns=2 and 1368 for the xpay
      equivalent
    */
    virtual long long flops() const
    {
      int mv_flops = (8 * in.Ncolor() - 2) * in.Ncolor(); // SU(3) matrix-vector flops
      int num_mv_multiply = in.Nspin() == 4 ? 2 : 1;
      int ghost_flops = (num_mv_multiply * mv_flops + 2*in.Ncolor()*in.Nspin());
      int xpay_flops = 2 * 2 * in.Ncolor() * in.Nspin(); // multiply and add per real component
      int num_dir = 2 * 4; // set to 4-d since we take care of 5-d fermions in derived classes where necessary
      int pack_flops = (in.Nspin() == 4 ? 2 * in.Nspin() / 2 * in.Ncolor()
                                        : 0); // only flops if spin projecting

      long long flops_ = 0;

      // FIXME - should we count the xpay flops in the derived kernels
      // since some kernels require the xpay in the exterior (preconditiond
      // clover)

      switch(arg.kernel_type) {
      case EXTERIOR_KERNEL_X:
      case EXTERIOR_KERNEL_Y:
      case EXTERIOR_KERNEL_Z:
      case EXTERIOR_KERNEL_T:
        flops_ = (ghost_flops + (arg.xpay ? xpay_flops : xpay_flops/2)) * 2 * in.GhostFace()[arg.kernel_type];
        break;
      case EXTERIOR_KERNEL_ALL:
        {
          long long ghost_sites = 2 * (in.GhostFace()[0]+in.GhostFace()[1]+in.GhostFace()[2]+in.GhostFace()[3]);
          flops_ = (ghost_flops + (arg.xpay ? xpay_flops : xpay_flops/2)) * ghost_sites;
          break;
        }
      case INTERIOR_KERNEL:
        if (arg.pack_threads) {
          flops_ += pack_flops * arg.nParity * in.getDslashConstant().Ls *
                    arg.pack_threads;
        }
      case KERNEL_POLICY:
        {
          long long sites = in.Volume();
          flops_ += (num_dir * (in.Nspin() / 4) * in.Ncolor() *
                         in.Nspin() + // spin project (=0 for staggered)
                     num_dir * num_mv_multiply *
                         mv_flops + // SU(3) matrix-vector multiplies
                     ((num_dir - 1) * 2 * in.Ncolor() * in.Nspin())) *
                    sites; // accumulation
          if (arg.xpay) flops_ += xpay_flops * sites;

          if (arg.kernel_type == KERNEL_POLICY)
            break;
          // now correct for flops done by exterior kernel
          long long ghost_sites = 0;
          for (int d=0; d<4; d++) if (arg.commDim[d]) ghost_sites += 2 * in.GhostFace()[d];
          flops_ -= ghost_flops * ghost_sites;

          break;
        }
      }

      return flops_;
    }

    virtual long long bytes() const
    {
      int gauge_bytes = arg.reconstruct * in.Precision();
      bool isFixed = (in.Precision() == sizeof(short) || in.Precision() == sizeof(char)) ? true : false;
      int spinor_bytes = 2 * in.Ncolor() * in.Nspin() * in.Precision() + (isFixed ? sizeof(float) : 0);
      int proj_spinor_bytes = in.Nspin() == 4 ? spinor_bytes / 2 : spinor_bytes;
      int ghost_bytes = (proj_spinor_bytes + gauge_bytes) +
                        2 * spinor_bytes; // 2 since we have to load the partial
      int num_dir = 2 * 4; // set to 4-d since we take care of 5-d fermions in
                           // derived classes where necessary
      int pack_bytes =
          2 * ((in.Nspin() == 4 ? in.Nspin() / 2 : in.Nspin()) + in.Nspin()) *
          in.Ncolor() * in.Precision();
      if (isFixed)
        pack_bytes += 2 * sizeof(float); // 2 is from input and output

      long long bytes_ = 0;

      switch(arg.kernel_type) {
      case EXTERIOR_KERNEL_X:
      case EXTERIOR_KERNEL_Y:
      case EXTERIOR_KERNEL_Z:
      case EXTERIOR_KERNEL_T:
        bytes_ = ghost_bytes * 2 * in.GhostFace()[arg.kernel_type];
        break;
      case EXTERIOR_KERNEL_ALL:
        {
          long long ghost_sites = 2 * (in.GhostFace()[0]+in.GhostFace()[1]+in.GhostFace()[2]+in.GhostFace()[3]);
          bytes_ += ghost_bytes * ghost_sites;
          break;
        }
      case INTERIOR_KERNEL:
        if (arg.pack_threads) {
          bytes_ += pack_bytes * arg.nParity * in.getDslashConstant().Ls *
                    arg.pack_threads;
        }
      case KERNEL_POLICY:
        {
          long long sites = in.Volume();
          bytes_ += (num_dir * gauge_bytes +
                     ((num_dir - 2) * spinor_bytes + 2 * proj_spinor_bytes) +
                     spinor_bytes) *
                    sites;
          if (arg.xpay) bytes_ += spinor_bytes;

          if (arg.kernel_type == KERNEL_POLICY) break;
          // now correct for bytes done by exterior kernel
          long long ghost_sites = 0;
          for (int d=0; d<4; d++) if (arg.commDim[d]) ghost_sites += 2*in.GhostFace()[d];
          bytes_ -= ghost_bytes * ghost_sites;

          break;
        }
      }
      return bytes_;
    }

  };

} // namespace quda
