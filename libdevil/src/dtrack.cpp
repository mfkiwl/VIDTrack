/*
 * Copyright (c) 2015  Juan M. Falquez,
 *                     University of Colorado - Boulder
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <devil/dtrack.h>

#include <miniglog/logging.h>

/////////////////////////////////////////////////////////////////////////////
inline float interp(
    float              x,            // Input: X coordinate.
    float              y,            // Input: Y coordinate.
    const float*       image_ptr,    // Input: Pointer to image.
    const unsigned int image_width,  // Input: Image width.
    const unsigned int image_height  // Input: Image height.
    )
{
  if (!((x >= 0) && (y >= 0) && (x <= image_width-2)
        && (y <= image_height-2))) {
    LOG(ERROR) << "Bad point: " << x << ", " << y;
  }

  x = std::max(std::min(x, static_cast<float>(image_width)-2.0f), 2.0f);
  y = std::max(std::min(y, static_cast<float>(image_height)-2.0f), 2.0f);

  const int    px  = static_cast<int>(x);  /* top-left corner */
  const int    py  = static_cast<int>(y);
  const float  ax  = x-px;
  const float  ay  = y-py;
  const float  ax1 = 1.0f-ax;
  const float  ay1 = 1.0f-ay;

  const float* p0  = image_ptr+(image_width*py)+px;

  float        p1  = p0[0];
  float        p2  = p0[1];
  float        p3  = p0[image_width];
  float        p4  = p0[image_width+1];

  p1 *= ay1;
  p2 *= ay1;
  p3 *= ay;
  p4 *= ay;
  p1 += p3;
  p2 += p4;
  p1 *= ax1;
  p2 *= ax;

  return p1+p2;
}

/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
class PoseRefine
{
public:
  ///////////////////////////////////////////////////////////////////////////
  PoseRefine(
      const cv::Mat&           live_grey,
      const cv::Mat&           ref_grey,
      const cv::Mat&           ref_depth,
      const Eigen::Matrix3d&   Klg,
      const Eigen::Matrix3d&   Krg,
      const Eigen::Matrix3d&   Krd,
      const Eigen::Matrix4d&   Tgd,
      const Eigen::Matrix4d&   Tlr,
      const Eigen::Matrix3x4d& KlgTlr,
      float                    norm_param,
      bool                     discard_saturated,
      float                    min_depth,
      float                    max_depth
      ):
    error(0),
    num_obs(0),
    live_grey_(live_grey),
    ref_grey_(ref_grey),
    ref_depth_(ref_depth),
    Klg_(Klg),
    Krg_(Krg),
    Krd_(Krd),
    Tgd_(Tgd),
    Tlr_(Tlr),
    KlgTlr_(KlgTlr),
    norm_param_(norm_param),
    discard_saturated_(discard_saturated),
    min_depth_(min_depth),
    max_depth_(max_depth)
  {
    hessian.setZero();
    LHS.setZero();
    RHS.setZero();
  }

  ///////////////////////////////////////////////////////////////////////////
  PoseRefine(
      const PoseRefine& x,
      tbb::split
      ):
    error(0),
    num_obs(0),
    live_grey_(x.live_grey_),
    ref_grey_(x.ref_grey_),
    ref_depth_(x.ref_depth_),
    Klg_(x.Klg_),
    Krg_(x.Krg_),
    Krd_(x.Krd_),
    Tgd_(x.Tgd_),
    Tlr_(x.Tlr_),
    KlgTlr_(x.KlgTlr_),
    norm_param_(x.norm_param_),
    discard_saturated_(x.discard_saturated_),
    min_depth_(x.min_depth_),
    max_depth_(x.max_depth_)
  {
    hessian.setZero();
    LHS.setZero();
    RHS.setZero();
  }

  ///////////////////////////////////////////////////////////////////////////
  void operator()(const tbb::blocked_range<size_t>& r)
  {
    // Local pointer for optimization apparently.
    for (size_t ii = r.begin(); ii != r.end(); ++ii) {
      const unsigned int u = ii%ref_depth_.cols;
      const unsigned int v = ii/ref_depth_.cols;

      // 2d point in reference depth camera.
      Eigen::Vector2d pr_d;
      pr_d << u, v;

      // Get depth.
      const float depth = ref_depth_.at<float>(v, u);

      // Check if depth is NAN.
      if (depth != depth) {
        continue;
      }

      if ((depth <= min_depth_) || (depth >= max_depth_)) {
        continue;
      }

      // 3d point in reference depth camera.
      Eigen::Vector4d hPr_d;
      hPr_d(0) = depth*(pr_d(0)-Krd_(0,2))/Krd_(0,0);
      hPr_d(1) = depth*(pr_d(1)-Krd_(1,2))/Krd_(1,1);
      hPr_d(2) = depth;
      hPr_d(3) = 1;

      // 3d point in reference grey camera (homogenized).
      // If depth and grey cameras are aligned, Tgd_ = I4.
      const Eigen::Vector4d hPr_g = Tgd_*hPr_d;

      // Project to reference grey camera's image coordinate.
      Eigen::Vector2d pr_g;
      pr_g(0) = (hPr_g(0)*Krg_(0,0)/hPr_g(2))+Krg_(0,2);
      pr_g(1) = (hPr_g(1)*Krg_(1,1)/hPr_g(2))+Krg_(1,2);

      // Check if point is out of bounds.
      if ((pr_g(0) < 2) || (pr_g(0) >= ref_grey_.cols-3) || (pr_g(1) < 2)
          || (pr_g(1) >= ref_grey_.rows-3)) {
        continue;
      }

      // Homogenized 3d point in live grey camera.
      const Eigen::Vector4d hPl_g = Tlr_*hPr_g;

      // Project to live grey ccamera's image coordinate.
      Eigen::Vector2d pl_g;
      pl_g(0) = (hPl_g(0)*Klg_(0,0)/hPl_g(2))+Klg_(0,2);
      pl_g(1) = (hPl_g(1)*Klg_(1,1)/hPl_g(2))+Klg_(1,2);

      // Check if point is out of bounds.
      if ((pl_g(0) < 2) || (pl_g(0) >= live_grey_.cols-3) || (pl_g(1) < 2)
          || (pl_g(1) >= live_grey_.rows-3)) {
        continue;
      }

      // Get intensities.
      const float Il =
          interp(pl_g(0), pl_g(1),
                 reinterpret_cast<float*>(live_grey_.data), live_grey_.cols,
                 live_grey_.rows);
      const float Ir =
          interp(pr_g(0), pr_g(1),
                 reinterpret_cast<float*>(ref_grey_.data), ref_grey_.cols,
                 ref_grey_.rows);

      // Discard under/over-saturated pixels.
      if (discard_saturated_) {
        if ((Il == 0) || (Il == 1.0) || (Ir == 0) || (Ir == 1.0)) {
          continue;
        }
      }

      // Calculate error.
      const double y = Il-Ir;


      ///-------------------- Forward Compositional
      // Image derivative.
      const float Il_xr =
          interp(pl_g(0)+1, pl_g(1),
                 reinterpret_cast<float*>(live_grey_.data), live_grey_.cols,
                 live_grey_.rows);
      const float Il_xl =
          interp(pl_g(0)-1, pl_g(1),
                 reinterpret_cast<float*>(live_grey_.data), live_grey_.cols,
                 live_grey_.rows);
      const float Il_yu =
          interp(pl_g(0), pl_g(1)-1,
                 reinterpret_cast<float*>(live_grey_.data), live_grey_.cols,
                 live_grey_.rows);
      const float Il_yd =
          interp(pl_g(0), pl_g(1)+1,
                 reinterpret_cast<float*>(live_grey_.data), live_grey_.cols,
                 live_grey_.rows);

      Eigen::Matrix<double, 1, 2> dIl;
      dIl << (Il_xr-Il_xl)/2.0, (Il_yd-Il_yu)/2.0;


      ///-------------------- Inverse Compositional
      // Image derivative.
      const float Ir_xr =
          interp(pr_g(0)+1, pr_g(1),
                 reinterpret_cast<float*>(ref_grey_.data), ref_grey_.cols,
                 ref_grey_.rows);
      const float Ir_xl =
          interp(pr_g(0)-1, pr_g(1),
                 reinterpret_cast<float*>(ref_grey_.data), ref_grey_.cols,
                 ref_grey_.rows);
      const float Ir_yu =
          interp(pr_g(0), pr_g(1)-1,
                 reinterpret_cast<float*>(ref_grey_.data), ref_grey_.cols,
                 ref_grey_.rows);
      const float Ir_yd =
          interp(pr_g(0), pr_g(1)+1,
                 reinterpret_cast<float*>(ref_grey_.data), ref_grey_.cols,
                 ref_grey_.rows);

      Eigen::Matrix<double, 1, 2> dIr;
      dIr << (Ir_xr-Ir_xl)/2.0, (Ir_yd-Ir_yu)/2.0;



      // Projection & dehomogenization derivative.
      Eigen::Vector3d KlPl = Klg_*hPl_g.head(3);

      Eigen::Matrix2x3d dPl;
      dPl << 1.0/KlPl(2), 0, -KlPl(0)/(KlPl(2)*KlPl(2)), 0, 1.0/KlPl(2),
          -KlPl(1)/(KlPl(2)*KlPl(2));

      const Eigen::Vector4d dIesm_dPl_KlgTlr = ((dIl+dIr)/2.0)*dPl*KlgTlr_;

      // J = dIesm_dPl_KlgTlr * gen_i * Pr
      Eigen::Vector6d J;
      J << dIesm_dPl_KlgTlr(0),
          dIesm_dPl_KlgTlr(1),
          dIesm_dPl_KlgTlr(2),
          -dIesm_dPl_KlgTlr(1)*hPr_g(2) + dIesm_dPl_KlgTlr(2)*hPr_g(1),
          +dIesm_dPl_KlgTlr(0)*hPr_g(2) - dIesm_dPl_KlgTlr(2)*hPr_g(0),
          -dIesm_dPl_KlgTlr(0)*hPr_g(1) + dIesm_dPl_KlgTlr(1)*hPr_g(0);



      ///-------------------- Robust Norm
      //      const double w = 1.0;
      const double w = _NormTukey(y, norm_param_);
      //      const double w = _NormL1(y, NormC);

      hessian     += J*J.transpose();
      LHS         += J*J.transpose()*w;
      RHS         += J*y*w;
      error       += y*y;
      num_obs++;
    }
  }

  ///////////////////////////////////////////////////////////////////////////
  void join(const PoseRefine& other)
  {
    LHS         += other.LHS;
    RHS         += other.RHS;
    hessian     += other.hessian;
    error       += other.error;
    num_obs     += other.num_obs;
  }

private:
  ///////////////////////////////////////////////////////////////////////////
  inline double _NormTukey(double r,
                           double c)
  {
    const double absr   = fabs(r);
    const double roc    = r/c;
    const double omroc2 = 1.0f-roc*roc;

    return (absr <= c) ? omroc2*omroc2 : 0.0f;
  }

  ///////////////////////////////////////////////////////////////////////////
  inline double _NormL1(double r,
                        double)
  {
    const double absr = fabs(r);

    return (absr == 0) ? 1.0f : 1.0f/absr;
  }

  ///
  ///////////////////////////////////////////////////////////////////////////

public:
  Eigen::Matrix6d   LHS;
  Eigen::Vector6d   RHS;
  Eigen::Matrix6d   hessian;
  double            error;
  unsigned int      num_obs;

private:
  cv::Mat           live_grey_;
  cv::Mat           ref_grey_;
  cv::Mat           ref_depth_;
  Eigen::Matrix3d   Klg_;
  Eigen::Matrix3d   Krg_;
  Eigen::Matrix3d   Krd_;
  Eigen::Matrix4d   Tgd_;
  Eigen::Matrix4d   Tlr_;
  Eigen::Matrix3x4d KlgTlr_;
  float             norm_param_;
  bool              discard_saturated_;
  float             min_depth_;
  float             max_depth_;
};


/////////////////////////////////////////////////////////////////////////////
DTrack::DTrack(unsigned int pyramid_levels) :
  kPyramidLevels(pyramid_levels),
  tbb_scheduler_(tbb::task_scheduler_init::deferred)
{
  tbb_scheduler_.initialize();
}

///////////////////////////////////////////////////////////////////////////
DTrack::~DTrack()
{
  tbb_scheduler_.terminate();
}

///////////////////////////////////////////////////////////////////////////
void DTrack::SetParams(
    const calibu::CameraModelGeneric<double>& live_grey_cmod,
    const calibu::CameraModelGeneric<double>& ref_grey_cmod,
    const calibu::CameraModelGeneric<double>& ref_depth_cmod,
    const Sophus::SE3d&                       Tgd
    )
{
  // Store scaled camera models (to avoid recomputing).
  for (size_t ii = 0; ii < kPyramidLevels; ++ii) {
    live_grey_cam_model_.push_back(_ScaleCM(live_grey_cmod, ii));
    ref_grey_cam_model_.push_back(_ScaleCM(ref_grey_cmod, ii));
    ref_depth_cam_model_.push_back(_ScaleCM(ref_depth_cmod, ii));
  }

  // Copy reference camera's depth-grey transform.
  Tgd_ = Tgd;
  LOG(INFO) << "Tgd is: " << Tgd.log().transpose() << std::endl;
}

///////////////////////////////////////////////////////////////////////////
void DTrack::SetKeyframe(
    const cv::Mat& ref_grey,  // Input: Reference image (float format, normalized).
    const cv::Mat& ref_depth  // Input: Reference depth (float format, meters).
    )
{
  // Build pyramids.
  cv::buildPyramid(ref_grey, ref_grey_pyramid_, kPyramidLevels);
  cv::buildPyramid(ref_depth, ref_depth_pyramid_, kPyramidLevels);
}

///////////////////////////////////////////////////////////////////////////
double DTrack::Estimate(
    const cv::Mat&            live_grey,    // Input: Live image (float format, normalized).
    Sophus::SE3Group<double>& Trl,          // Input/Output: Transform between grey cameras (input is hint).
    Eigen::Matrix6d&          covariance,   // Output: Covariance
    bool                      use_pyramid   // Input: Options.
    )
{
  // TODO(jfalquez) Pass this options in Config method to avoid re-initializing.
  // Options.
  const double norm_c            = 0.04;
  const bool   discard_saturated = true;
  const float  min_depth         = 0.01;
  const float  max_depth         = 100.0;

  // Set pyramid max-iterations and full estimate mask.
  std::vector<bool>         vec_full_estimate  = {1, 1, 1, 0};
  std::vector<unsigned int> vec_max_iterations = {1, 2, 3, 4};

  if (use_pyramid == false) {
    vec_max_iterations = {3, 0, 0, 0};
  }

  CHECK_EQ(vec_full_estimate.size(), kPyramidLevels);
  CHECK_EQ(vec_max_iterations.size(), kPyramidLevels);

  // Build live pyramid.
  cv::buildPyramid(live_grey, live_grey_pyramid_, kPyramidLevels);

  // Aux variables.
  Eigen::Matrix6d   hessian;
  Eigen::Matrix6d   LHS;
  Eigen::Vector6d   RHS;
  double            squared_error;
  double            number_observations;
  double            last_error = FLT_MAX;

  // Iterate through pyramid levels.
  for (int pyramid_lvl = kPyramidLevels-1; pyramid_lvl >= 0; pyramid_lvl--) {
    const cv::Mat& live_grey_img = live_grey_pyramid_[pyramid_lvl];
    const cv::Mat& ref_grey_img  = ref_grey_pyramid_[pyramid_lvl];
    const cv::Mat& ref_depth_img = ref_depth_pyramid_[pyramid_lvl];

    const calibu::CameraModelGeneric<double>& live_grey_cmod =
        ref_grey_cam_model_[pyramid_lvl];
    const calibu::CameraModelGeneric<double>& ref_grey_cmod =
        ref_grey_cam_model_[pyramid_lvl];
    const calibu::CameraModelGeneric<double>& ref_depth_cmod =
        ref_depth_cam_model_[pyramid_lvl];

    // Reset error.
    last_error = FLT_MAX;

    // Set pyramid norm parameter.
    const double norm_c_pyr = norm_c*(pyramid_lvl+1);

    for (unsigned int num_iters = 0; num_iters < vec_max_iterations[pyramid_lvl]; ++num_iters) {
      // Reset.
      hessian.setZero();
      LHS.setZero();
      RHS.setZero();

      // Reset error.
      squared_error        = 0;
      number_observations  = 0;

      // Inverse transform.
      const Sophus::SE3d      Tlr    = Trl.inverse();

      const Eigen::Matrix3d   Klg    = live_grey_cmod.K();
      const Eigen::Matrix3d   Krg    = ref_grey_cmod.K();
      const Eigen::Matrix3d   Krd    = ref_depth_cmod.K();

      const Eigen::Matrix3x4d KlgTlr = Klg*Tlr.matrix3x4();

      // Launch TBB.
      PoseRefine pose_ref(live_grey_img, ref_grey_img, ref_depth_img, Klg,
                          Krg, Krd, Tgd_.matrix(), Tlr.matrix(), KlgTlr,
                          norm_c_pyr, discard_saturated, min_depth, max_depth);

      tbb::parallel_reduce(tbb::blocked_range<size_t>(0,
                                                      ref_depth_img.cols*ref_depth_img.rows, 10000), pose_ref);

      LHS                  = pose_ref.LHS;
      RHS                  = pose_ref.RHS;
      squared_error        = pose_ref.error;
      number_observations  = pose_ref.num_obs;

      // Solution.
      Eigen::Vector6d X;

      // Check if we are solving only for rotation, or full estimate.
      if (vec_full_estimate[pyramid_lvl]) {
        // Decompose matrix.
        Eigen::FullPivLU<Eigen::Matrix<double, 6, 6> > lu_JTJ(LHS);

        // Check degenerate system.
        if (lu_JTJ.rank() < 6) {
          LOG(WARNING) << "[@L:" << pyramid_lvl << " I:"
                       << num_iters << "] LS trashed. Rank deficient!";
        }

        X = -(lu_JTJ.solve(RHS));
      } else {
        // Extract rotation information only.
        Eigen::Matrix3d rLHS = LHS.block<3, 3>(3, 3);
        Eigen::Vector3d rRHS = RHS.tail(3);

        Eigen::FullPivLU<Eigen::Matrix<double, 3, 3> > lu_JTJ(rLHS);

        // Check degenerate system.
        if (lu_JTJ.rank() < 3) {
          LOG(WARNING) << "[@L:" << pyramid_lvl << " I:"
                       << num_iters << "] LS trashed. Rank deficient!";
        }

        Eigen::Vector3d rX;
        rX = -(lu_JTJ.solve(rRHS));

        // Pack solution.
        X.setZero();
        X.tail(3) = rX;
      }

      // Get RMSE.
      const double new_error = sqrt(squared_error/number_observations);

      if (new_error < last_error) {
        // Update error.
        last_error = new_error;

        // Update Trl.
        Trl = (Tlr*Sophus::SE3Group<double>::exp(X)).inverse();

        if (pyramid_lvl == 1) {
          LOG(INFO) << "[@L:" << pyramid_lvl << " I:"
                    << num_iters << "] Update is: " << Trl.log().transpose();
        }

        // Store hessian.
        if (pyramid_lvl == 0) {
          hessian = pose_ref.hessian;
        }

        if (X.norm() < 1e-5) {
          VLOG(1) << "[@L:" << pyramid_lvl << " I:"
                  << num_iters << "] Update is too small. Breaking early!";
          break;
        }
      } else {
        VLOG(1) << "[@L:" << pyramid_lvl << " I:"
                << num_iters << "] Error is increasing. Breaking early!";
        break;
      }
    }
  }

  // Set covariance output.
  covariance = hessian.inverse();

  return last_error;
}

///////////////////////////////////////////////////////////////////////////
inline calibu::CameraModelGeneric<double> DTrack::_ScaleCM(
    calibu::CameraModelGeneric<double> cam_model,
    unsigned int                       level
    )
{
  const float scale = 1.0f/(1 << level);

  return cam_model.Scaled(scale);
}