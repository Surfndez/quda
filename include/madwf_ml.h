#pragma once

#include <vector>
#include <random>
#include <unordered_map>

#include <color_spinor_field.h>
#include <blas_quda.h>
#include <madwf_transfer.h>
#include <polynomial.h>

namespace quda
{

  struct MADWFacc {

    using TrainingFloat = float;

    // The parameters to be trained.
    using Tp = madwf_ml::TrainingParameter<TrainingFloat>;
    Tp device_param;

    // The diagonal component to suppress/lift the zero modes.
    double mu;
    int Ls_base;

    int null_maxiter;
    double null_tol;
    int train_maxiter;

    // persistent buffers for reuse.
    ColorSpinorField *forward_tmp = nullptr;
    ColorSpinorField *backward_tmp = nullptr;

    bool param_load;
    bool param_save;

    char param_infile[256];
    char param_outfile[256];

    // Has device_param been trained?
    bool trained = false;

    QudaPrecision prec_precondition;

    static std::unordered_map<std::string, std::vector<TrainingFloat>> host_training_param_cache; // empty map

    MADWFacc(const SolverParam &solve_param) :
      mu(solve_param.madwf_diagonal_suppressor),
      Ls_base(solve_param.madwf_ls),
      null_maxiter(solve_param.madwf_null_maxiter),
      null_tol(solve_param.madwf_null_tol),
      train_maxiter(solve_param.madwf_train_maxiter),
      param_load(solve_param.madwf_param_load == QUDA_BOOLEAN_TRUE),
      param_save(solve_param.madwf_param_save == QUDA_BOOLEAN_TRUE),
      prec_precondition(solve_param.precision_precondition)
    {
      strcpy(param_infile, solve_param.madwf_param_infile);
      strcpy(param_outfile, solve_param.madwf_param_outfile);

      printfQuda("Launching MADWFacc ... \n");
      printfQuda("madwf_mu            = %.4f\n", mu);
      printfQuda("madwf_ls            = %d\n", Ls_base);
      printfQuda("madwf_null_maxiter  = %d\n", null_maxiter);
      printfQuda("madwf_null_tol      = %.2f\n", null_tol);
      printfQuda("madwf_train_maxiter = %d\n", train_maxiter);
    }

    ~MADWFacc()
    {
      if (forward_tmp) { delete forward_tmp; }
      if (backward_tmp) { delete backward_tmp; }
    }

    void fill_random(std::vector<TrainingFloat> &v)
    {
      static std::random_device rd;
      // the good rng
      static std::mt19937 rng(23ul * comm_rank());
      // The gaussian distribution
      static std::normal_distribution<double> n(0., 1.);

      for (auto &x : v) { x = 1e-1 * n(rng); }
    }

    template <class Base> void apply(Base base, ColorSpinorField &out, const ColorSpinorField &in)
    {
      madwf_ml::transfer_5d_hh(*forward_tmp, in, device_param, false);
      base(*backward_tmp, *forward_tmp);
      madwf_ml::transfer_5d_hh(out, *backward_tmp, device_param, true);

      blas::axpy(mu, const_cast<ColorSpinorField &>(in), out);
    }

    template <class Ref, class Base>
    double cost(const Ref &ref, Base base, ColorSpinorField &out, const ColorSpinorField &in)
    {

      ColorSpinorParam csParam(in);
      cudaColorSpinorField tmp1(csParam);
      cudaColorSpinorField tmp2(csParam);

      apply(base, tmp1, in);
      ref(tmp2, tmp1);

      blas::copy(out, in);

      // M * T^ * A * T * phi - phi
      return blas::xmyNorm(tmp2, out);
    }

// Uncomment the following marco to enable tuning mu (suppressor).
// #define TUNE_SUPPRESSOR

    template <class Ref, class Base, class Null>
    void train(const Ref &ref, Base base, Null &null, const ColorSpinorField &in)
    {

#if 1
      constexpr int complex_matrix_size = 16; // spin by spin
#else
      constexpr int complex_matrix_size = 2; // chiral
#endif

      int Ls = in.X(4);
      int param_size = Ls * Ls_base * complex_matrix_size * 2;
      std::vector<TrainingFloat> host_param(param_size);

      if (param_load) {
        char param_file_name[512];
        // Note that all ranks load from the same file.
        sprintf(param_file_name, "/madwf_trained_param_rank_%05d_ls_%02d_%02d_mu_%.3f.dat", 0, Ls, Ls_base, mu);
        std::string param_file_name_str(param_file_name);
        auto search_cache = host_training_param_cache.find(param_file_name_str);
        if (search_cache != host_training_param_cache.end()) {
          host_param = search_cache->second;
          printfQuda("Training params loaded from CACHE.\n");
        } else {
          // the parameter is not in cache: load from file system.
          std::string save_param_path(param_infile);
          save_param_path += param_file_name_str;
          FILE *fp = fopen(save_param_path.c_str(), "rb");
          if (!fp) { errorQuda("Unable to open file %s\n", save_param_path.c_str()); }
          size_t fread_count = fread(host_param.data(), sizeof(float), host_param.size(), fp);
          fclose(fp);
          if (fread_count != host_param.size()) {
            errorQuda("Unable to load training params from %s (%lu neq %lu).\n", save_param_path.c_str(), fread_count,
                      host_param.size());
          }
          host_training_param_cache.insert({param_file_name_str, host_param});
          printf("Rank %05d: Training params loaded from FILE %s ... \n", comm_rank(), save_param_path.c_str());
          comm_barrier();
          printfQuda("All ranks loaded.\n");
        }
        device_param.resize(param_size); // 2 for complex
        device_param.from_host(host_param);
        trained = true;

        ColorSpinorParam csParam(in);
        csParam.x[4] = Ls_base;
        csParam.create = QUDA_NULL_FIELD_CREATE;
        csParam.setPrecision(prec_precondition);

        forward_tmp = new cudaColorSpinorField(csParam);
        backward_tmp = new cudaColorSpinorField(csParam);

        return;
      }

      ColorSpinorParam csParam(in);
      cudaColorSpinorField null_x(csParam);
      cudaColorSpinorField null_b(csParam);

      auto rng = new RNG(null_b, 1234);
      rng->Init();

      printfQuda("Generating Null Space Vectors ... \n");
      spinorNoise(null_b, *rng, QUDA_NOISE_GAUSS);

      rng->Release();
      delete rng;

      std::vector<ColorSpinorField *> B(16);
      csParam.setPrecision(prec_precondition);
      for (auto &pB : B) { pB = new cudaColorSpinorField(csParam); }

      // TODO: Currently this only work for PreconCG.
      static_cast<PreconCG &>(null)(null_x, null_b, B, null_maxiter, null_tol);
      for (auto &pB : B) { blas::ax(5e3 / sqrt(blas::norm2(*pB)), *pB); }

      saveTuneCache();

      bool global_reduction = commGlobalReduction();
      commGlobalReductionSet(false);

      cudaColorSpinorField chi(csParam);
      cudaColorSpinorField tmp(csParam);
      cudaColorSpinorField theta(csParam);
      cudaColorSpinorField lambda(csParam);
      cudaColorSpinorField Mchi(csParam);

      double residual = 0.0;
      int count = 0;
      for (const auto &phi : B) {
        residual += blas::norm2(*phi);
        printfQuda("reference dslash norm %03d = %8.4e\n", count, blas::norm2(*phi));
        count++;
      }
      printfQuda("reference dslash norm = %8.4e\n", residual);

      csParam.x[4] = Ls_base;
      csParam.create = QUDA_ZERO_FIELD_CREATE;

      cudaColorSpinorField ATchi(csParam);
      cudaColorSpinorField ATphi(csParam);
      cudaColorSpinorField ADphi(csParam);

      cudaColorSpinorField ATMchi(csParam);

      forward_tmp = new cudaColorSpinorField(csParam);
      backward_tmp = new cudaColorSpinorField(csParam);

      fill_random(host_param);

      device_param.resize(param_size);
      device_param.from_host(host_param);

      Tp d1(param_size);
      Tp d2(param_size);
      Tp P(param_size);
      Tp D_old(param_size);

#ifdef TUNE_SUPPRESSOR
      double pmu = 0.0;
#endif

      TrainingFloat alpha;
      TrainingFloat b = 0.8;
      printfQuda("beta          = %.3f\n", b);
      printfQuda("training mu   = %.3f\n", mu);
      for (int iteration = 0; iteration < train_maxiter; iteration++) {

        Tp D(param_size);
#ifdef TUNE_SUPPRESSOR
        double dmu = 0.0;
#endif
        double chi2 = 0.0;
        std::array<double, 5> a = {};

        for (const auto &phi : B) {
          chi2 += cost(ref, base, chi, *phi);
          // ATx(ATphi, *phi, T);
          madwf_ml::transfer_5d_hh(*forward_tmp, *phi, device_param, false);
          base(ATphi, *forward_tmp);

          // inner_dslash(Mchi, chi);
          ref(Mchi, chi);

          // ATx(ATMchi, Mchi, T);
          madwf_ml::transfer_5d_hh(*forward_tmp, Mchi, device_param, false);
          base(ATMchi, *forward_tmp);

          // d1 = A * T * phi -x- M * chi
          madwf_ml::tensor_5d_hh(ATphi, Mchi, d1);
          // d2 = A * T * M * phi -x- phi
          madwf_ml::tensor_5d_hh(ATMchi, *phi, d2);

          madwf_ml::axpby(D, 2.0f, d1, 2.0f, d2);
#ifdef TUNE_SUPPRESSOR
          dmu += 2.0 * blas::reDotProduct(Mchi, *phi);
#endif
        }

        madwf_ml::axpby(P, (b - 1), P, (1 - b), D);
#ifdef TUNE_SUPPRESSOR
        pmu = b * pmu + (1 - b) * dmu;
#endif

        chi2 = 0.0;
        // line search
        for (const auto &phi : B) {

          double ind_chi2 = cost(ref, base, chi, *phi);
          chi2 += ind_chi2;

          // ATx(ATphi, *phi, T);
          madwf_ml::transfer_5d_hh(*forward_tmp, *phi, device_param, false);
          base(ATphi, *forward_tmp);

          // D' * A * T * phi
          madwf_ml::transfer_5d_hh(theta, ATphi, P, true);

          // ATx(ADphi, *phi, P);
          madwf_ml::transfer_5d_hh(*forward_tmp, *phi, P, false);
          base(ADphi, *forward_tmp);

          // T' * A * D * phi
          madwf_ml::transfer_5d_hh(tmp, ADphi, device_param, true);
          // theta
          blas::axpy(1.0, theta, tmp);
#ifdef TUNE_SUPPRESSOR
          blas::axpy(pmu, *phi, tmp);
#endif

          // inner_dslash(theta, tmp);
          ref(theta, tmp);

          // lambda = D' * A * D * phi
          madwf_ml::transfer_5d_hh(tmp, ADphi, P, true);

          // inner_dslash(lambda, tmp);
          ref(lambda, tmp);

          std::vector<ColorSpinorField *> lhs {&chi, &theta, &lambda};
          std::vector<ColorSpinorField *> rhs {&chi, &theta, &lambda};
          Complex dot[9];
          blas::cDotProduct(dot, lhs, rhs);

          a[0] += dot[0].real();
          a[1] += -2.0 * dot[1].real();
          a[2] += dot[4].real() + 2.0 * dot[2].real();
          a[3] += -2.0 * dot[5].real();
          a[4] += dot[8].real();
        }

        std::array<double, 4> coeffs = {4.0 * a[4], 3.0 * a[3], 2.0 * a[2], a[1]};
        auto rs = cubic_formula(coeffs);

        alpha = 0;
        double root_min = poly4(a, 0);
        for (auto r : rs) {
          double eval = poly4(a, r);
          if (root_min > eval) {
            root_min = eval;
            alpha = r;
          }
        }

        madwf_ml::axpby(device_param, 0.0f, device_param, -alpha, P);
#ifdef TUNE_SUPPRESSOR
        mu -= alpha * pmu;
#endif

        printfQuda("grad min iter %03d: %04d chi2 = %8.4e, chi2 %% = %8.4e, alpha = %+8.4e, mu = %+8.4e\n", comm_rank(),
                   iteration, chi2, chi2 / residual, alpha, mu);

      }

      trained = true;

      printfQuda("Training finished ...\n");
      count = 0;
      for (const auto &phi : B) {
        double ind_chi2 = cost(ref, base, chi, *phi);
        double phi2 = blas::norm2(*phi);
        printfQuda("chi2 %03d %% = %8.4e, phi2 = %8.4e\n", count, ind_chi2 / phi2, phi2);
        count++;
      }

      if (param_save) {
        host_param = device_param.to_host();

        std::string save_param_path(param_outfile);
        char cstring[512];
        sprintf(cstring, "/madwf_trained_param_rank_%05d_ls_%02d_%02d_mu_%.3f.dat", comm_rank(), Ls, Ls_base, mu);
        save_param_path += std::string(cstring);
        FILE *fp = fopen(save_param_path.c_str(), "w");
        size_t fwrite_count = fwrite(host_param.data(), sizeof(TrainingFloat), host_param.size(), fp);
        fclose(fp);
        if (fwrite_count != host_param.size()) {
          errorQuda("Unable to write trained parameters to %s (%lu neq %lu).\n", save_param_path.c_str(), fwrite_count,
                    host_param.size());
        }
        printfQuda("Trained parameters saved to %s ...\n", save_param_path.c_str());

        comm_barrier();
      }

      // Destroy all dynamically allocated stuff.
      for (auto &pB : B) { delete pB; }
      commGlobalReductionSet(global_reduction);
    }
  };

} // namespace quda