#include <copy_color_spinor.cuh>

namespace quda {
  
  void copyGenericColorSpinorQH(ColorSpinorField &dst, const ColorSpinorField &src, 
				QudaFieldLocation location, void *Dst, void *Src, 
				void *dstNorm, void *srcNorm) {
#if (QUDA_PRECISION & 2) && (QUDA_PRECISION & 1)
    CopyGenericColorSpinor<3>(dst, src, location, (int8_t*)Dst, (short*)Src, (float*)dstNorm, (float*)srcNorm);
#else
    errorQuda("QUDA_PRECISION=%d does not enable precision combination %d %d", QUDA_PRECISION, dst.Precision(), src.Precision());
#endif
  }  

} // namespace quda
