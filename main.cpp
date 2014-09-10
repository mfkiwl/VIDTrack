#include <unistd.h>
#include <deque>

#include <Eigen/Eigen>
#include <sophus/sophus.hpp>
#include <opencv2/opencv.hpp>

#include <HAL/Utils/GetPot>
#include <HAL/Utils/TicToc.h>
#include <HAL/Camera/CameraDevice.h>
#include <pangolin/pangolin.h>
#include <SceneGraph/SceneGraph.h>
#include <calibu/Calibu.h>
#include <calibu/calib/LocalParamSe3.h>
#include <calibu/calib/CostFunctionAndParams.h>
#include <ba/BundleAdjuster.h>
#include <ba/Types.h>
#include <ba/InterpolationBuffer.h>

#include "AuxGUI/AnalyticsView.h"
#include "AuxGUI/Timer.h"
#include "AuxGUI/TimerView.h"
#include "AuxGUI/GLPath.h"

#include "dtrack.h"
#include "ceres_dense_ba.h"


///////////////////////////////////////////////////////////////////////////
/// Generates a "heat map" based on an error image provided.
cv::Mat GenerateHeatMap(const cv::Mat& input)
{
  cv::Mat output(input.rows, input.cols, CV_8UC3);

  // Get min/max to normalize.
  double min, max;
  cv::minMaxIdx(input, &min, &max);
  const double mean = cv::mean(input).val[0];
  max = 3*mean;
  for (int vv = 0; vv < input.rows; ++vv) {
    for (int uu = 0; uu < input.cols; ++uu) {
      float n_val = (input.at<float>(vv, uu) - min) / (max - min);
      if (n_val < 0.5) {
        output.at<cv::Vec3b>(vv, uu) = cv::Vec3b(255*n_val, 0, 128);
      } else {
        output.at<cv::Vec3b>(vv, uu) = cv::Vec3b(255, 0, 128*n_val);
      }
    }
  }
  return output;
}

/////////////////////////////////////////////////////////////////////////////
/// Convert greyscale image to float and normalizes.
inline cv::Mat ConvertAndNormalize(const cv::Mat& in)
{
  cv::Mat out;
  in.convertTo(out, CV_32FC1);
  out /= 255.0;
  return out;
}



/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////
int main(int argc, char** argv)
{
  std::cout << "Starting DEVIL ..." << std::endl;

  ///----- Initialize Camera.
  GetPot cl_args(argc, argv);
  if (!cl_args.search("-cam")) {
    std::cerr << "Camera arguments missing!" << std::endl;
    exit(EXIT_FAILURE);
  }

  hal::Camera camera(cl_args.follow("", "-cam"));

  const int image_width = camera.Width();
  const int image_height = camera.Height();
  std::cout << "- Image Dimensions: " << image_width <<
               "x" << image_height << std::endl;


  ///----- Set up GUI.
  pangolin::CreateGlutWindowAndBind("DEVIL", 1600, 800);

  // Set up panel.
  const unsigned int panel_size = 180;
  pangolin::CreatePanel("ui").SetBounds(0, 1, 0, pangolin::Attach::Pix(panel_size));
  pangolin::Var<bool>  ui_camera_follow("ui.Camera Follow", true, true);
  pangolin::Var<bool>  ui_reset("ui.Reset", true, false);
  pangolin::Var<bool>  ui_use_gt_poses("ui.Use GT Poses", false, true);
  pangolin::Var<bool>  ui_use_constant_velocity("ui.Use Const Vel Model", false, true);

  // Set up container.
  pangolin::View& container = pangolin::CreateDisplay();
  container.SetBounds(0, 1, pangolin::Attach::Pix(panel_size), 0.65);
  container.SetLayout(pangolin::LayoutEqual);
  pangolin::DisplayBase().AddDisplay(container);

  // Set up timer.
  Timer     timer;
  TimerView timer_view;
  timer_view.SetBounds(0.5, 1, 0.65, 1.0);
  pangolin::DisplayBase().AddDisplay(timer_view);
  timer_view.InitReset();

  // Set up analytics.
  std::map<std::string, float>  analytics;
  AnalyticsView                 analytics_view;
  analytics_view.SetBounds(0, 0.5, 0.65, 1.0);
  pangolin::DisplayBase().AddDisplay(analytics_view);
  analytics_view.InitReset();

  // Set up 3D view for container.
  SceneGraph::GLSceneGraph gl_graph;
  SceneGraph::GLSceneGraph::ApplyPreferredGlSettings();

  // Reset background color to black.
  glClearColor(0, 0, 0, 1);

  // Add path.
  GLPath gl_path;
  gl_graph.AddChild(&gl_path);
  std::vector<Sophus::SE3d>& gl_path_vec = gl_path.GetPathRef();

  // Add axis.
  SceneGraph::GLAxis gl_axis;
  gl_graph.AddChild(&gl_axis);

  // Add grid.
  SceneGraph::GLGrid gl_grid(50, 1);
  gl_graph.AddChild(&gl_grid);

  pangolin::View view_3d;
  const double far = 10*1000;
  const double near = 1E-3;

  pangolin::OpenGlRenderState stacks3d(
        pangolin::ProjectionMatrix(640, 480, 420, 420, 320, 240, near, far),
        pangolin::ModelViewLookAt(-5, 0, -8, 0, 0, 0, pangolin::AxisNegZ)
        );

  view_3d.SetHandler(new SceneGraph::HandlerSceneGraph(gl_graph, stacks3d))
      .SetDrawFunction(SceneGraph::ActivateDrawFunctor(gl_graph, stacks3d));

  // Add all subviews to container.
  SceneGraph::ImageView image_view;
  image_view.SetAspect(640.0 / 480.0);
  container.AddDisplay(image_view);

  SceneGraph::ImageView depth_view;
  container.AddDisplay(depth_view);

  container.AddDisplay(view_3d);

  // GUI aux variables.
  bool capture_flag;
  bool paused = true;
  bool step_once = false;


  ///----- Load camera model.
  calibu::CameraRig rig;
  if (camera.GetDeviceProperty(hal::DeviceDirectory).empty() == false) {
    std::cout<<"- Loaded camera: " <<
               camera.GetDeviceProperty(hal::DeviceDirectory) + '/'
               + cl_args.follow("cameras.xml", "-cmod") << std::endl;
    rig = calibu::ReadXmlRig(camera.GetDeviceProperty(hal::DeviceDirectory)
                             + '/' + cl_args.follow("cameras.xml", "-cmod"));
  } else {
    rig = calibu::ReadXmlRig(cl_args.follow("cameras.xml", "-cmod"));
  }
  Eigen::Matrix3f K = rig.cameras[0].camera.K().cast<float>();
  Eigen::Matrix3f Kinv = K.inverse();
  std::cout << "-- K is: " << std::endl << K << std::endl;

  ///----- Init DTrack stuff.
  cv::Mat keyframe_image, keyframe_depth;
  DTrack dtrack;
  dtrack.Init();
  dtrack.SetParams(rig.cameras[0].camera, rig.cameras[0].camera,
      rig.cameras[0].camera, Sophus::SE3d());

  ///----- Init BA stuff.
  typedef ba::ImuMeasurementT<double>               ImuMeasurement;
  ba::InterpolationBufferT<ImuMeasurement, double>  imu_buffer;
  ba::BundleAdjuster<double,0,9,0>                  bundle_adjuster;
  ba::Options<double>                               options;
  options.trust_region_size = 100000;
  bundle_adjuster.Init(options);

  ///----- Load file of ground truth poses (required).
  std::vector<Sophus::SE3d> poses;
  {
    std::string pose_file = cl_args.follow("", "-poses");
    if (pose_file.empty()) {
      std::cerr << "- NOTE: No poses file given. It is required!" << std::endl;
      exit(EXIT_FAILURE);
    }
    pose_file = camera.GetDeviceProperty(hal::DeviceDirectory) + "/" + pose_file;
    FILE* fd = fopen(pose_file.c_str(), "r");
    Eigen::Matrix<double, 6, 1> pose;
    float x, y, z, p, q, r;

    std::cout << "- Loading pose file: '" << pose_file << "'" << std::endl;
    if (cl_args.search("-V")) {
      // Vision convention.
      std::cout << "- NOTE: File is being read in VISION frame." << std::endl;
    } else if (cl_args.search("-T")) {
      // Tsukuba convention.
      std::cout << "- NOTE: File is being read in TSUKUBA frame." << std::endl;
    } else {
      // Robotics convention (default).
      std::cout << "- NOTE: File is being read in ROBOTICS frame." << std::endl;
    }

    while (fscanf(fd, "%f\t%f\t%f\t%f\t%f\t%f", &x, &y, &z, &p, &q, &r) != EOF) {
      pose(0) = x;
      pose(1) = y;
      pose(2) = z;
      pose(3) = p;
      pose(4) = q;
      pose(5) = r;

      Sophus::SE3d T(SceneGraph::GLCart2T(pose));

      // Flag to load poses as a particular convention.
      if (cl_args.search("-V")) {
        // Vision convention.
        poses.push_back(T);
      } else if (cl_args.search("-T")) {
        // Tsukuba convention.
        Eigen::Matrix3d tsukuba_convention;
        tsukuba_convention << -1,  0,  0,
                               0, -1,  0,
                               0,  0, -1;
        Sophus::SO3d tsukuba_convention_sophus(tsukuba_convention);
        poses.push_back(calibu::ToCoordinateConvention(T,
                                        tsukuba_convention_sophus.inverse()));
      } else {
        // Robotics convention (default).
        poses.push_back(calibu::ToCoordinateConvention(T,
                                        calibu::RdfRobotics.inverse()));
      }
    }
    std::cout << "- NOTE: " << poses.size() << " poses loaded." << std::endl;
    fclose(fd);
  }

  ///----- Load file of IMU measurements (required).
  std::vector<Eigen::Matrix<double, 7, 1>> imu;
  {
    std::string imu_file = cl_args.follow("", "-imu");
    if (imu_file.empty()) {
      std::cerr << "- NOTE: No IMU file given. It is required!" << std::endl;
      exit(EXIT_FAILURE);
    }
    imu_file = camera.GetDeviceProperty(hal::DeviceDirectory) + "/" + imu_file;
    FILE* fd = fopen(imu_file.c_str(), "r");
    Eigen::Matrix<double, 7, 1> imu_measurement;
    float timestamp, accelX, accelY, accelZ, gyroX, gyroY, gyroZ;

    std::cout << "- Loading IMU measurements file: '" << imu_file << "'" << std::endl;

    while (fscanf(fd, "%f\t%f\t%f\t%f\t%f\t%f\t%f", &timestamp, &accelX, &accelY,
                  &accelZ, &gyroX, &gyroY, &gyroZ) != EOF) {
      imu_measurement(0) = timestamp;
      imu_measurement(1) = accelX;
      imu_measurement(2) = accelY;
      imu_measurement(3) = accelZ;
      imu_measurement(4) = gyroX;
      imu_measurement(5) = gyroY;
      imu_measurement(6) = gyroZ;

      imu.push_back(imu_measurement);
    }
    std::cout << "- NOTE: " << imu.size() << " IMU measurements loaded." << std::endl;
    fclose(fd);
  }

  ///----- Load image timestamps.
  std::vector<double> image_timestamps;
  {
    std::string timestamps_file = cl_args.follow("", "-timestamps");
    if (timestamps_file.empty()) {
      std::cerr << "- NOTE: No timestamps file given. It is required!" << std::endl;
      exit(EXIT_FAILURE);
    }
    timestamps_file = camera.GetDeviceProperty(hal::DeviceDirectory) + "/" + timestamps_file;
    FILE* fd = fopen(timestamps_file.c_str(), "r");
    double timestamp;

    std::cout << "- Loading timestamps file: '" << timestamps_file << "'" << std::endl;

    while (fscanf(fd, "%lf", &timestamp) != EOF) {
      image_timestamps.push_back(timestamp);
    }
    fclose(fd);
    std::cout << "- NOTE: " << image_timestamps.size() << " timestamps loaded." << std::endl;
  }


  ///----- Register callbacks.
  // Hide/Show panel.
  pangolin::RegisterKeyPressCallback('~', [&](){
    static bool fullscreen = true;
    fullscreen = !fullscreen;
    if (fullscreen) {
      container.SetBounds(0, 1, pangolin::Attach::Pix(panel_size), 0.65);
    } else {
      container.SetBounds(0, 1, 0, 1);
    }
    analytics_view.Show(fullscreen);
    timer_view.Show(fullscreen);
    pangolin::Display("ui").Show(fullscreen);
  });

  // Container view handler.
  const char keyShowHide[] = {'1','2','3','4','5','6','7','8','9','0'};
  const char keySave[]     = {'!','@','#','$','%','^','&','*','(',')'};
  for (int ii = 0; ii < container.NumChildren(); ii++) {
    pangolin::RegisterKeyPressCallback(keyShowHide[ii], [&container,ii]() {
      container[ii].ToggleShow(); });
    pangolin::RegisterKeyPressCallback(keySave[ii], [&container,ii]() {
      container[ii].SaveRenderNow("screenshot", 4); });
  }

  pangolin::RegisterKeyPressCallback(' ', [&paused] { paused = !paused; });
  pangolin::RegisterKeyPressCallback(pangolin::PANGO_SPECIAL + GLUT_KEY_RIGHT,
                                     [&step_once] {
                                        step_once = !step_once; });
  pangolin::RegisterKeyPressCallback(pangolin::PANGO_CTRL + 'r',
                                     [&ui_reset] {
                                        ui_reset = true; });


  ///----- Init general variables.
  unsigned int current_frame = 0;
  Sophus::SE3d current_pose;
  Sophus::SE3d pose_estimate;
  std::shared_ptr<pb::ImageArray> images = pb::ImageArray::Create();


  /////////////////////////////////////////////////////////////////////////////
  ///---- MAIN LOOP
  ///

  while (!pangolin::ShouldQuit()) {

    // Start timer.
    timer.Tic();

    ///----- Init reset ...
    if (pangolin::Pushed(ui_reset)) {
      // Reset timer and analytics.
      timer_view.InitReset();
      analytics_view.InitReset();

      // Reset path.
      gl_path_vec.clear();
      // Path expects poses in robotic convetion.
      {
        current_pose = Sophus::SE3d();
        Sophus::SO3d& rotation = current_pose.so3();
        rotation = calibu::RdfRobotics;
        gl_path_vec.push_back(current_pose);
      }

      // Re-initialize camera.
      if (!camera.GetDeviceProperty(hal::DeviceDirectory).empty()) {
       camera = hal::Camera(cl_args.follow("", "-cam"));
      }

      // Reset frame counter.
      current_frame = 0;

      // Capture first image.
      capture_flag = camera.Capture(*images);
      cv::Mat current_image = ConvertAndNormalize(images->at(0)->Mat());

      // Reset reference image for DTrack.
      keyframe_image = current_image;
      keyframe_depth = images->at(1)->Mat();

      // Add initial pose for BA.
//      bundle_adjuster.AddPose(current_pose , true, timestamp);

      // Increment frame counter.
      current_frame++;
    }


    ///----- Step forward ...
    if (!paused || pangolin::Pushed(step_once)) {
      //  Capture the new image.
      capture_flag = camera.Capture(*images);

      if (capture_flag == false) {
        paused = true;
      } else {
        // Convert to float and normalize.
        cv::Mat current_image = ConvertAndNormalize(images->at(0)->Mat());

        // Get pose for this image.
        timer.Tic("DTrack");

        // Reset pose estimate to identity if no constant velocity model is used.
        if (!ui_use_constant_velocity) {
          pose_estimate = Sophus::SE3d();
        }

        // RGBD pose estimation.
        dtrack.SetKeyframe(keyframe_image, keyframe_depth);
        double dtrack_error = dtrack.Estimate(current_image, pose_estimate);
        analytics["DTrack RMS"] = dtrack_error;

        // Calculate pose error.
        Sophus::SE3d gt_pose = poses[current_frame-1].inverse()
            * poses[current_frame];
        analytics["DTrack Error"] =
            (Sophus::SE3::log(pose_estimate.inverse() * gt_pose).head(3).norm()
             / Sophus::SE3::log(gt_pose).head(3).norm()) * 100.0;
        timer.Toc("DTrack");

        // If using ground-truth poses, override pose estimate with GT pose.
        if (ui_use_gt_poses) {
          pose_estimate = gt_pose;
        }

        // Update pose.
        current_pose = current_pose * pose_estimate;
        gl_path_vec.push_back(pose_estimate);

        // Reset reference image for DTrack.
        keyframe_image = current_image;
        keyframe_depth = images->at(1)->Mat();

        // Increment frame counter.
        current_frame++;

        // Update analytics.
        analytics_view.Update(analytics);
      }
    }


    /////////////////////////////////////////////////////////////////////////////
    ///---- Render
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (capture_flag) {
      image_view.SetImage(images->at(0)->data(), image_width, image_height,
                          GL_RGB8, GL_LUMINANCE, GL_UNSIGNED_BYTE);

      depth_view.SetImage(images->at(1)->data(), image_width, image_height,
                          GL_RGB8, GL_LUMINANCE, GL_FLOAT, true);
    }

    gl_axis.SetPose(current_pose.matrix());

    if (ui_camera_follow) {
      stacks3d.Follow(current_pose.matrix());
    }

    // Sleep a bit.
    usleep(1e6/60.0);

    // Stop timer and update.
    timer.Toc();
    timer_view.Update(10, timer.GetNames(3), timer.GetTimes(3));

    pangolin::FinishFrame();
  }

  return 0;
}
