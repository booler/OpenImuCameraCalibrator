#include <opencv2/aruco/charuco.hpp>

#include "OpenCameraCalibrator/utils/intrinsic_initializer.h"
#include "OpenCameraCalibrator/utils/utils.h"

namespace OpenCamCalib {

bool initialize_pinhole_camera(
    const std::vector<theia::FeatureCorrespondence2D3D> &correspondences,
    const theia::RansacParameters &ransac_params,
    theia::RansacSummary &ransac_summary, Eigen::Matrix3d &rotation,
    Eigen::Vector3d &position, double &focal_length, const bool verbose) {

  theia::UncalibratedAbsolutePose pose_linear;
  // use -> cv::initCameraMatrix2D()
  const bool success = theia::EstimateUncalibratedAbsolutePose(
      ransac_params, theia::RansacType::RANSAC, correspondences, &pose_linear,
      &ransac_summary);
  std::cout << "Estimated focal length: " << pose_linear.focal_length
            << std::endl;
  std::cout << "Number of Ransac inliers: " << ransac_summary.inliers.size()
            << std::endl;

  rotation = pose_linear.rotation;
  position = pose_linear.position;
  focal_length = pose_linear.focal_length;
  if (ransac_summary.inliers.size() < 10)
    return false;
  return success;
}

bool initialize_radial_undistortion_camera(
    const std::vector<theia::FeatureCorrespondence2D3D> &correspondences,
    const theia::RansacParameters &ransac_params,
    theia::RansacSummary &ransac_summary, const int img_cols,
    Eigen::Matrix3d &rotation, Eigen::Vector3d &position, double &focal_length,
    double &radial_distortion, const bool verbose) {

  if (correspondences.size() <= 4) {
    return false;
  }
  theia::RadialDistUncalibratedAbsolutePoseMetaData meta_data;
  theia::RadialDistUncalibratedAbsolutePose pose_division_undist;
  meta_data.max_focal_length = 0.5 * img_cols + 150.0;
  meta_data.min_focal_length = 0.5 * img_cols - 150.0;
  const bool success = theia::EstimateRadialDistUncalibratedAbsolutePose(
      ransac_params, theia::RansacType::RANSAC, correspondences, meta_data,
      &pose_division_undist, &ransac_summary);
  if (verbose) {
      std::cout << "Estimated focal length: " << pose_division_undist.focal_length
                << std::endl;
      std::cout << "Estimated radial distortion: "
                << pose_division_undist.radial_distortion << std::endl
                << std::endl;
      std::cout << "Number of Ransac inliers: " << ransac_summary.inliers.size()
                << std::endl;
  }
  rotation = pose_division_undist.rotation;
  position = -pose_division_undist.rotation.transpose() * pose_division_undist.translation;
  radial_distortion = pose_division_undist.radial_distortion;
  focal_length = pose_division_undist.focal_length;
  if (ransac_summary.inliers.size() < 10)
    return false;
  return success;
}

// took that initialization from basalt
// https://gitlab.com/VladyslavUsenko/basalt/-/blob/master/src/calibration/calibraiton_helper.cpp
bool initialize_doublesphere_model(
    const std::vector<theia::FeatureCorrespondence2D3D> &correspondences,
    std::vector<int> charuco_ids,
    cv::Ptr<cv::aruco::CharucoBoard> &charucoboard,
    const theia::RansacParameters &ransac_params, const int img_cols,
    const int img_rows, theia::RansacSummary &ransac_summary,
    Eigen::Matrix3d &rotation, Eigen::Vector3d &position,
    double &focal_length, const bool verbose) {
  // First, initialize the image center at the center of the image.

  aligned_map<int, Eigen::Vector2d> id_to_corner;
  for (size_t i = 0; i < charuco_ids.size(); i++) {
    id_to_corner[charuco_ids[i]] = correspondences[i].feature;
  }

  const double _xi = 1.0;
  const double _cu = img_cols / 2.0 - 0.5;
  const double _cv = img_rows / 2.0 - 0.5;

  // Initialize some temporaries needed.
  double gamma0 = 0.0;
  double min_reproj_error = std::numeric_limits<double>::max();
  bool success = false;

  // Now we try to find a non-radial line to initialize the focal length
  const size_t target_cols = charucoboard->getChessboardSize().width;
  const size_t target_rows = charucoboard->getChessboardSize().height;

  for (int r = 0; r < target_rows; ++r) {
    aligned_vector<Eigen::Vector4d> P;

    for (int c = 0; c < target_cols; ++c) {
      int corner_id = (r * target_cols + c);

      if (id_to_corner.find(corner_id) != id_to_corner.end()) {
        const Eigen::Vector2d imagePoint = id_to_corner[corner_id];
        P.emplace_back(imagePoint[0], imagePoint[1], 0.5,
                       -0.5 * (imagePoint[0] * imagePoint[0] +
                               imagePoint[1] * imagePoint[1]));
      }
    }
    const int MIN_CORNERS = 8;
    // MIN_CORNERS is an arbitrary threshold for the number of corners
    if (P.size() > MIN_CORNERS) {
      // Resize P to fit with the count of valid points.

      Eigen::Map<Eigen::Matrix4Xd> P_mat((double *)P.data(), 4, P.size());

      // std::cerr << "P_mat\n" << P_mat.transpose() << std::endl;

      Eigen::MatrixXd P_mat_t = P_mat.transpose();

      Eigen::JacobiSVD<Eigen::MatrixXd> svd(P_mat_t, Eigen::ComputeThinU |
                                                         Eigen::ComputeThinV);

      // std::cerr << "U\n" << svd.matrixU() << std::endl;
      // std::cerr << "V\n" << svd.matrixV() << std::endl;
      // std::cerr << "singularValues\n" << svd.singularValues() <<
      // std::endl;

      Eigen::Vector4d C = svd.matrixV().col(3);
      // std::cerr << "C\n" << C.transpose() << std::endl;
      // std::cerr << "P*res\n" << P_mat.transpose() * C << std::endl;

      double t = C(0) * C(0) + C(1) * C(1) + C(2) * C(3);
      if (t < 0) {
        continue;
      }

      // check that line image is not radial
      double d = sqrt(1.0 / t);
      double nx = C(0) * d;
      double ny = C(1) * d;
      if (hypot(nx, ny) > 0.95) {
        // std::cerr << "hypot(nx, ny) " << hypot(nx, ny) << std::endl;
        continue;
      }

      double nz = sqrt(1.0 - nx * nx - ny * ny);
      double gamma = fabs(C(2) * d / nz);

      // undistort points with intrinsic guess
      theia::Camera cam;
      cam.SetCameraIntrinsicsModelType(
          theia::CameraIntrinsicsModelType::DOUBLE_SPHERE);
      double *intrinsics = cam.mutable_intrinsics();
      intrinsics[theia::DoubleSphereCameraModel::InternalParametersIndex::
                     FOCAL_LENGTH] = 0.5 * gamma;
      intrinsics[theia::DoubleSphereCameraModel::InternalParametersIndex::
                     PRINCIPAL_POINT_X] = _cu;
      intrinsics[theia::DoubleSphereCameraModel::InternalParametersIndex::
                     PRINCIPAL_POINT_Y] = _cv;
      intrinsics[theia::DoubleSphereCameraModel::InternalParametersIndex::
                     ASPECT_RATIO] = 1.0;
      intrinsics[theia::DoubleSphereCameraModel::InternalParametersIndex::XI] =
          0.5 * _xi;
      intrinsics
          [theia::DoubleSphereCameraModel::InternalParametersIndex::ALPHA] =
              0.0;

      std::vector<theia::FeatureCorrespondence2D3D> correspondences_new =
          correspondences;

      for (auto &cor : correspondences_new) {
        cor.feature =
            cam.PixelToNormalizedCoordinates(cor.feature).hnormalized();
      }

      theia::CalibratedAbsolutePose pose;
      theia::EstimateCalibratedAbsolutePose(
          ransac_params, theia::RansacType::RANSAC, correspondences_new, &pose,
          &ransac_summary);
      cam.SetPosition(pose.position);
      cam.SetOrientationFromRotationMatrix(pose.rotation);

      double repro_error = 0.0;
      int in_image = 0;
      for (int i = 0; i < correspondences_new.size(); ++i) {
        Eigen::Vector2d pixel;
        cam.ProjectPoint(correspondences_new[i].world_point.homogeneous(),
                         &pixel);
        if (pixel(0) >= 0.0 && pixel(1) >= 0.0 && pixel(0) < img_cols &&
            pixel(1) < img_rows) {
          repro_error += (pixel - correspondences[i].feature).squaredNorm();
          in_image++;
        }
      }
      if (verbose) {
          std::cout << "numReprojected " << in_image << " reprojErr "
                    << repro_error / in_image << std::endl;
      }
      if (in_image > MIN_CORNERS) {
        double avg_reproj_error = repro_error / in_image;

        if (avg_reproj_error < min_reproj_error && avg_reproj_error < 1000.0) {
          min_reproj_error = avg_reproj_error;
          gamma0 = gamma;
          success = true;
          rotation = pose.rotation;
          position = pose.position;
          focal_length = 0.5 * gamma0;
          if (verbose) {
            std::cout << "new min_reproj_error: " << min_reproj_error
                    << std::endl;
          }

        } // if clause
      }   // if in_image
    }     // if P.size()
  }       // for target cols
  if (ransac_summary.inliers.size() < 10)
    return false;
  return success;
} // for target rows

//
} // namespace OpenCamCalib
