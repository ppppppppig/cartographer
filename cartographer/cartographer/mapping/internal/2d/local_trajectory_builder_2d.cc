/*
 * Copyright 2016 The Cartographer Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cartographer/mapping/internal/2d/local_trajectory_builder_2d.h"

#include <limits>
#include <memory>

#include "cartographer/common/make_unique.h"
#include "cartographer/metrics/family_factory.h"
#include "cartographer/sensor/range_data.h"

namespace cartographer {
namespace mapping {

bool have_done_match_full_submap = false;

static auto* kLocalSlamLatencyMetric = metrics::Gauge::Null();
static auto* kFastCorrelativeScanMatcherScoreMetric =
    metrics::Histogram::Null();
static auto* kCeresScanMatcherCostMetric = metrics::Histogram::Null();
static auto* kScanMatcherResidualDistanceMetric = metrics::Histogram::Null();
static auto* kScanMatcherResidualAngleMetric = metrics::Histogram::Null();

LocalTrajectoryBuilder2D::LocalTrajectoryBuilder2D(
    const proto::LocalTrajectoryBuilderOptions2D& options,
    const std::vector<std::string>& expected_range_sensor_ids)
    : options_(options),
      active_submaps_(options.submaps_options()),
      motion_filter_(options_.motion_filter_options()),
      real_time_correlative_scan_matcher_(
          options_.real_time_correlative_scan_matcher_options()),
      ceres_scan_matcher_(options_.ceres_scan_matcher_options()),
      range_data_collator_(expected_range_sensor_ids) {}

LocalTrajectoryBuilder2D::~LocalTrajectoryBuilder2D() {}

sensor::RangeData
LocalTrajectoryBuilder2D::TransformToGravityAlignedFrameAndFilter(
    const transform::Rigid3f& transform_to_gravity_aligned_frame,
    const sensor::RangeData& range_data) const {
  const sensor::RangeData cropped =
      sensor::CropRangeData(sensor::TransformRangeData(
                                range_data, transform_to_gravity_aligned_frame),
                            options_.min_z(), options_.max_z());
  return sensor::RangeData{
      cropped.origin,
      sensor::VoxelFilter(options_.voxel_filter_size()).Filter(cropped.returns),
      sensor::VoxelFilter(options_.voxel_filter_size()).Filter(cropped.misses)};
}

std::unique_ptr<transform::Rigid2d> LocalTrajectoryBuilder2D::ScanMatch(
    const common::Time time, const transform::Rigid2d& pose_prediction,
    const sensor::RangeData& gravity_aligned_range_data) {
      //从地图中取旧图
  std::shared_ptr<const Submap2D> matching_submap =
      active_submaps_.submaps().front();
  // The online correlative scan matcher will refine the initial estimate for
  // the Ceres scan matcher.
  transform::Rigid2d initial_ceres_pose = pose_prediction;

  //构建体素滤波对输入点云过滤
  sensor::AdaptiveVoxelFilter adaptive_voxel_filter(
      options_.adaptive_voxel_filter_options());
  const sensor::PointCloud filtered_gravity_aligned_point_cloud =
      adaptive_voxel_filter.Filter(gravity_aligned_range_data.returns);
  if (filtered_gravity_aligned_point_cloud.empty()) {
    return nullptr;
  }


  if (options_.use_online_correlative_scan_matching()) {
    CHECK_EQ(options_.submaps_options().grid_options_2d().grid_type(),
             proto::GridOptions2D_GridType_PROBABILITY_GRID);
    
    double score = real_time_correlative_scan_matcher_.Match(
        pose_prediction, filtered_gravity_aligned_point_cloud,
        *static_cast<const ProbabilityGrid*>(matching_submap->grid()),
        &initial_ceres_pose);//通过此方法获得较好初值

    kFastCorrelativeScanMatcherScoreMetric->Observe(score);
  }

  auto pose_observation = common::make_unique<transform::Rigid2d>();
  ceres::Solver::Summary summary;
  ceres_scan_matcher_.Match(pose_prediction.translation(), initial_ceres_pose,
                            filtered_gravity_aligned_point_cloud,
                            *matching_submap->grid(), pose_observation.get(),
                            &summary);//ceres_scan_matcher通过ceres快速获得定位
  if (pose_observation) {
    kCeresScanMatcherCostMetric->Observe(summary.final_cost);
    double residual_distance =
        (pose_observation->translation() - pose_prediction.translation())
            .norm();
    kScanMatcherResidualDistanceMetric->Observe(residual_distance);
    double residual_angle = std::abs(pose_observation->rotation().angle() -
                                     pose_prediction.rotation().angle());
    kScanMatcherResidualAngleMetric->Observe(residual_angle);
  }
  return pose_observation;
}

std::unique_ptr<LocalTrajectoryBuilder2D::MatchingResult>
LocalTrajectoryBuilder2D::AddRangeData(
    const std::string& sensor_id,
    const sensor::TimedPointCloudData& unsynchronized_data) {
      //将数据记录下来，并同步
  auto synchronized_data =
      range_data_collator_.AddRangeData(sensor_id, unsynchronized_data);
  if (synchronized_data.ranges.empty()) {
    LOG(INFO) << "Range data collator filling buffer.";
    return nullptr;
  }

//初始化位姿估计器
  const common::Time& time = synchronized_data.time;
  // Initialize extrapolator now if we do not ever use an IMU.
  if (!options_.use_imu_data()) {
    InitializeExtrapolator(time);
  }

  if (extrapolator_ == nullptr) {
    // Until we've initialized the extrapolator with our first IMU message, we
    // cannot compute the orientation of the rangefinder.
    LOG(INFO) << "Extrapolator not yet initialized.";
    return nullptr;
  }

//检查一下数据获取的时间
  CHECK(!synchronized_data.ranges.empty());
  // TODO(gaschler): Check if this can strictly be 0.
  CHECK_LE(synchronized_data.ranges.back().point_time[3], 0.f);
  const common::Time time_first_point =
      time +
      common::FromSeconds(synchronized_data.ranges.front().point_time[3]);
  if (time_first_point < extrapolator_->GetLastPoseTime()) {
    LOG(INFO) << "Extrapolator is still initializing.";
    return nullptr;
  }

//作为开始
  if (num_accumulated_ == 0) {
    accumulation_started_ = std::chrono::steady_clock::now();
  }


//range_data_poses记录各个时间段的pose
  std::vector<transform::Rigid3f> range_data_poses;
  range_data_poses.reserve(synchronized_data.ranges.size());
  bool warned = false;

  //检查是否有时间在位姿估计器初始化之前的
  for (const auto& range : synchronized_data.ranges) {
    common::Time time_point = time + common::FromSeconds(range.point_time[3]);
    if (time_point < extrapolator_->GetLastExtrapolatedTime()) {
      if (!warned) {
        LOG(ERROR)
            << "Timestamp of individual range data point jumps backwards from "
            << extrapolator_->GetLastExtrapolatedTime() << " to " << time_point;
        warned = true;
      }
      time_point = extrapolator_->GetLastExtrapolatedTime();
    }
    range_data_poses.push_back(
        extrapolator_->ExtrapolatePose(time_point).cast<float>());//通过测量时间过去位姿，这个位姿初始化器又是什么结构
  }


//若还没扫描过累计数据，则累计数据归零
  if (num_accumulated_ == 0) {
    // 'accumulated_range_data_.origin' is uninitialized until the last
    // accumulation.
    accumulated_range_data_ = sensor::RangeData{{}, {}, {}};
  }


  // Drop any returns below the minimum range and convert returns beyond the
  // maximum range into misses.
  for (size_t i = 0; i < synchronized_data.ranges.size(); ++i) {
    const Eigen::Vector4f& hit = synchronized_data.ranges[i].point_time;
    const Eigen::Vector3f origin_in_local =
        range_data_poses[i] *
        synchronized_data.origins.at(synchronized_data.ranges[i].origin_index);
    const Eigen::Vector3f hit_in_local = range_data_poses[i] * hit.head<3>();
    const Eigen::Vector3f delta = hit_in_local - origin_in_local;
    const float range = delta.norm();
    if (range >= options_.min_range()) {
      if (range <= options_.max_range()) {
        accumulated_range_data_.returns.push_back(hit_in_local);
      } else {
        accumulated_range_data_.misses.push_back(
            origin_in_local +
            options_.missing_data_ray_length() / range * delta);
      }
    }
  }
  ++num_accumulated_;//扫描过一次累计数据

  if (num_accumulated_ >= options_.num_accumulated_range_data()) {
    //当扫描次数超过某阈值时，调用位姿估计器计算重力方向，和累计位姿
    num_accumulated_ = 0;
    const transform::Rigid3d gravity_alignment = transform::Rigid3d::Rotation(
        extrapolator_->EstimateGravityOrientation(time));
    // TODO(gaschler): This assumes that 'range_data_poses.back()' is at time
    // 'time'.
    accumulated_range_data_.origin = range_data_poses.back().translation();
    return AddAccumulatedRangeData(
        time,
        TransformToGravityAlignedFrameAndFilter(
            gravity_alignment.cast<float>() * range_data_poses.back().inverse(),
            accumulated_range_data_),
        gravity_alignment);
  }
  return nullptr;
}


//时间，重力同步之后的数据，重力方向
std::unique_ptr<LocalTrajectoryBuilder2D::MatchingResult>
LocalTrajectoryBuilder2D::AddAccumulatedRangeData(
    const common::Time time,
    const sensor::RangeData& gravity_aligned_range_data,
    const transform::Rigid3d& gravity_alignment) {
  if (gravity_aligned_range_data.returns.empty()) {
    LOG(WARNING) << "Dropped empty horizontal range data.";
    return nullptr;
  }

  // Computes a gravity aligned pose prediction.
  //计算重力修正的位姿
  const transform::Rigid3d non_gravity_aligned_pose_prediction =
      extrapolator_->ExtrapolatePose(time);
  const transform::Rigid2d pose_prediction = transform::Project2D(
      non_gravity_aligned_pose_prediction * gravity_alignment.inverse());//这个pose_prediction可能是初值？

  // local map frame <- gravity-aligned frame
  //与map扫描匹配，得到新的位姿估计
  std::unique_ptr<transform::Rigid2d> pose_estimate_2d =
      ScanMatch(time, pose_prediction, gravity_aligned_range_data);
  if (pose_estimate_2d == nullptr) {
    LOG(WARNING) << "Scan matching failed.";
    return nullptr;
  }


// //my add
//   const int MIN_SCORE = 0.8;
//   if(have_done_match_full_submap){
//     //滤掉重力
//       sensor::AdaptiveVoxelFilter adaptive_voxel_filter(
//         options_.adaptive_voxel_filter_options());
//       const sensor::PointCloud filtered_gravity_aligned_point_cloud =
//         adaptive_voxel_filter.Filter(gravity_aligned_range_data.returns);
//     //获取匹配地图
//       std::shared_ptr<const Submap2D> matching_submap =
//           active_submaps_.submaps().front();
      
//     //计算当前匹配得分
//       double score =real_time_correlative_scan_matcher_.getscore(*pose_estimate_2d,filtered_gravity_aligned_point_cloud,*static_cast<const ProbabilityGrid*>(matching_submap->grid()));
//       if(score < MIN_SCORE){
//         LOG(ERROR) << "need ?";
//     }
//   }


//将更新的pose返回给估计器
  const transform::Rigid3d pose_estimate =
      transform::Embed3D(*pose_estimate_2d) * gravity_alignment;
  extrapolator_->AddPose(time, pose_estimate);


//将数据插入到map中
  sensor::RangeData range_data_in_local =
      TransformRangeData(gravity_aligned_range_data,
                         transform::Embed3D(pose_estimate_2d->cast<float>()));
  std::unique_ptr<InsertionResult> insertion_result =
      InsertIntoSubmap(time, range_data_in_local, gravity_aligned_range_data,
                       pose_estimate, gravity_alignment.rotation());
  auto duration = std::chrono::steady_clock::now() - accumulation_started_;
  kLocalSlamLatencyMetric->Set(
      std::chrono::duration_cast<std::chrono::seconds>(duration).count());
  return common::make_unique<MatchingResult>(
      MatchingResult{time, pose_estimate, std::move(range_data_in_local),
                     std::move(insertion_result)});
}

std::unique_ptr<LocalTrajectoryBuilder2D::InsertionResult>
LocalTrajectoryBuilder2D::InsertIntoSubmap(
    const common::Time time, const sensor::RangeData& range_data_in_local,
    const sensor::RangeData& gravity_aligned_range_data,
    const transform::Rigid3d& pose_estimate,
    const Eigen::Quaterniond& gravity_alignment) {
  
  //通过运动滤波器判断是否发生了运动，未发生则不插入
  if (motion_filter_.IsSimilar(time, pose_estimate)) {
    return nullptr;
  }

  // Querying the active submaps must be done here before calling
  // InsertRangeData() since the queried values are valid for next insertion.
  //将原submap存入insertion_submaps中，
  std::vector<std::shared_ptr<const Submap2D>> insertion_submaps;
  for (const std::shared_ptr<Submap2D>& submap : active_submaps_.submaps()) {
    insertion_submaps.push_back(submap);
  }


  //插入数据
  active_submaps_.InsertRangeData(range_data_in_local);


//对重力修正的扫描数据进行滤波
  sensor::AdaptiveVoxelFilter adaptive_voxel_filter(
      options_.loop_closure_adaptive_voxel_filter_options());
  const sensor::PointCloud filtered_gravity_aligned_point_cloud =
      adaptive_voxel_filter.Filter(gravity_aligned_range_data.returns);


//将插入结果返回
  return common::make_unique<InsertionResult>(InsertionResult{
      std::make_shared<const TrajectoryNode::Data>(TrajectoryNode::Data{
          time,
          gravity_alignment,
          filtered_gravity_aligned_point_cloud,
          {},  // 'high_resolution_point_cloud' is only used in 3D.
          {},  // 'low_resolution_point_cloud' is only used in 3D.
          {},  // 'rotational_scan_matcher_histogram' is only used in 3D.
          pose_estimate}),
      std::move(insertion_submaps)});
}

void LocalTrajectoryBuilder2D::AddImuData(const sensor::ImuData& imu_data) {
  CHECK(options_.use_imu_data()) << "An unexpected IMU packet was added.";
  InitializeExtrapolator(imu_data.time);
  extrapolator_->AddImuData(imu_data);
}

void LocalTrajectoryBuilder2D::AddOdometryData(
    const sensor::OdometryData& odometry_data) {
  if (extrapolator_ == nullptr) {
    // Until we've initialized the extrapolator we cannot add odometry data.
    LOG(INFO) << "Extrapolator not yet initialized.";
    return;
  }
  extrapolator_->AddOdometryData(odometry_data);
}

void LocalTrajectoryBuilder2D::InitializeExtrapolator(const common::Time time) {
  if (extrapolator_ != nullptr) {
    return;
  }
  // We derive velocities from poses which are at least 1 ms apart for numerical
  // stability. Usually poses known to the extrapolator will be further apart
  // in time and thus the last two are used.
  constexpr double kExtrapolationEstimationTimeSec = 0.001;
  // TODO(gaschler): Consider using InitializeWithImu as 3D does.
  extrapolator_ = common::make_unique<PoseExtrapolator>(
      ::cartographer::common::FromSeconds(kExtrapolationEstimationTimeSec),
      options_.imu_gravity_time_constant());
  extrapolator_->AddPose(time, transform::Rigid3d::Identity());
}

void LocalTrajectoryBuilder2D::RegisterMetrics(
    metrics::FamilyFactory* family_factory) {
  auto* latency = family_factory->NewGaugeFamily(
      "mapping_internal_2d_local_trajectory_builder_latency",
      "Duration from first incoming point cloud in accumulation to local slam "
      "result");
  kLocalSlamLatencyMetric = latency->Add({});
  auto score_boundaries = metrics::Histogram::FixedWidth(0.05, 20);
  auto* scores = family_factory->NewHistogramFamily(
      "mapping_internal_2d_local_trajectory_builder_scores",
      "Local scan matcher scores", score_boundaries);
  kFastCorrelativeScanMatcherScoreMetric =
      scores->Add({{"scan_matcher", "fast_correlative"}});
  auto cost_boundaries = metrics::Histogram::ScaledPowersOf(2, 0.01, 100);
  auto* costs = family_factory->NewHistogramFamily(
      "mapping_internal_2d_local_trajectory_builder_costs",
      "Local scan matcher costs", cost_boundaries);
  kCeresScanMatcherCostMetric = costs->Add({{"scan_matcher", "ceres"}});
  auto distance_boundaries = metrics::Histogram::ScaledPowersOf(2, 0.01, 10);
  auto* residuals = family_factory->NewHistogramFamily(
      "mapping_internal_2d_local_trajectory_builder_residuals",
      "Local scan matcher residuals", distance_boundaries);
  kScanMatcherResidualDistanceMetric =
      residuals->Add({{"component", "distance"}});
  kScanMatcherResidualAngleMetric = residuals->Add({{"component", "angle"}});
}

}  // namespace mapping
}  // namespace cartographer
