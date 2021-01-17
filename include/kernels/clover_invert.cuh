#include <clover_field_order.h>
#include <quda_matrix.h>
#include <linalg.cuh>
#include <reduction_kernel.h>

namespace quda
{

  template <typename store_t_, bool twist_> struct CloverInvertArg : public ReduceArg<double2> {
    using store_t = store_t_;
    using real = typename mapper<store_t>::type;
    static constexpr bool twist = twist_;
    static constexpr int nColor = N_COLORS;
    static constexpr int nSpin = 4;
    using Clover = typename clover_mapper<store_t>::type;

    dim3 threads; // number of active threads required
    Clover inverse;
    const Clover clover;
    bool compute_tr_log;
    real mu2;

    CloverInvertArg(CloverField &field, bool compute_tr_log) :
      ReduceArg<double2>(),
      threads(field.VolumeCB(), 2, 1),
      inverse(field, true),
      clover(field, false),
      compute_tr_log(compute_tr_log),
      mu2(field.Mu2())
    {
      if (!field.isNative()) errorQuda("Clover field %d order not supported", field.Order());
    }

    __device__ __host__ auto init() const { return zero<double2>(); }
  };

  template <typename Arg> struct InvertClover {

    using reduce_t = double2;
    Arg &arg;
    constexpr InvertClover(Arg &arg) : arg(arg) {}
    static constexpr const char *filename() { return KERNEL_FILE; }

    /**
       Use a Cholesky decomposition and invert the clover matrix
    */
    template <typename Reducer>
    __device__ __host__ inline reduce_t operator()(reduce_t &value, Reducer &r, int x_cb, int parity)
    {
      using real = typename Arg::real;
      constexpr int N = (Arg::nColor * Arg::nSpin) / 2;
      using Mat = HMatrix<real, N>;
      double trLogA = 0.0;

      for (int ch = 0; ch < 2; ch++) {
        Mat A = arg.clover(x_cb, parity, ch);
        A *= static_cast<real>(2.0); // factor of two is inherent to QUDA clover storage

        if (Arg::twist) { // Compute (T^2 + mu2) first, then invert
          A = A.square();
          A += arg.mu2;
        }

        // compute the Cholesky decomposition
        linalg::Cholesky<HMatrix, real, N> cholesky(A);

        // Accumulate trlogA
        if (arg.compute_tr_log)
          for (int j = 0; j < N; j++) trLogA += 2.0 * log(cholesky.D(j));

        Mat Ainv = static_cast<real>(0.5) * cholesky.invert(); // return full inverse
        arg.inverse(x_cb, parity, ch) = Ainv;
      }

      reduce_t result = zero<reduce_t>();
      parity ? result.y = trLogA : result.x = trLogA;
      return r(result, value);
    }

  };

} // namespace quda
