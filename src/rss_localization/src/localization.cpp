#include "ros/ros.h"
#include "std_msgs/String.h"
#include "std_msgs/ColorRGBA.h"
#include "nav_msgs/Odometry.h"
#include "geometry_msgs/PoseWithCovarianceStamped.h"
#include "geometry_msgs/PoseArray.h"
#include "visualization_msgs/MarkerArray.h"
#include "tf/transform_broadcaster.h"
#include "tf/transform_datatypes.h"
#include "tf/LinearMath/Transform.h"
#include "rss_grid_localization/util.h"
#include "rss_grid_localization/ScanProcessor.h"
#include "rss_grid_localization/MapHandler.h"
#include "rss_grid_localization/ParticleFilterStateEstimator.h"
#include "rss_grid_localization/OdometryMotionModel.h"
#include "rss_grid_localization/LidarMeasurementModel.h"

#define MAP_FRAME "map"
#define ROBOT_FRAME "base_footprint"

using namespace std;
using namespace std_msgs;
using namespace nav_msgs;
using namespace geometry_msgs;
using namespace sensor_msgs;
using namespace visualization_msgs;

using namespace rss;

Odometry lastUsedOdom;
Odometry lastReceivedOdom;
bool newAction = false;
bool firstAction = true;
std::vector<ParticleFilterStateEstimator::Particle> particle_buffer;
bool firstPublish = true;

void odomCallback(const Odometry::ConstPtr &msg) {
  newAction = true;
  lastReceivedOdom = *msg;
  if (firstAction) {
    lastUsedOdom = lastReceivedOdom;
    firstAction = false;
  }
}

bool newPose = false;
Pose rvizPose;

void rvizPoseCallback(const PoseWithCovarianceStamped::ConstPtr &msg) {
  rvizPose = msg->pose.pose;
  newPose = true;
}

Action getAction() {
  newAction = false;
  ros::Duration stamp = lastReceivedOdom.header.stamp - lastUsedOdom.header.stamp;
  lastUsedOdom = lastReceivedOdom;
  return {lastReceivedOdom.twist.twist, stamp.toSec()};
}

bool shouldResample(const Action &action) {
  static const double trans_threshold = 0.0005;
  static const double rot_threshold = 0.04;
  return (abs(action.trans) > trans_threshold || abs(action.rot) > rot_threshold);
}

nav_msgs::Odometry smoothPose(ParticleFilterStateEstimator::Particle particle) {
  if (firstPublish) {
    particle_buffer.resize(5);
    for (auto p: particle_buffer) {
      p = particle;
    }
    firstPublish = false;
  } else {
    particle_buffer.erase(particle_buffer.begin());
    particle_buffer.push_back(particle);
  }

  // now calculate the moving average of the poses
  double x = 0.0;
  double y = 0.0;
  double theta = 0.0;
  for (int i = 0; i < particle_buffer.size(); ++i) {
    double weight = (i + 1) / ((pow(particle_buffer.size(), 2) + particle_buffer.size()) / 2);
    x += particle_buffer[i].pose.x * weight;
    y += particle_buffer[i].pose.y * weight;
    theta += particle_buffer[i].pose.theta * weight;
  }

  nav_msgs::Odometry odom_msg;
  odom_msg.header.stamp = ros::Time::now();
  odom_msg.header.frame_id = MAP_FRAME;
  odom_msg.pose.pose.position.x = x;
  odom_msg.pose.pose.position.y = y;

  tf::Quaternion q;
  q.setRPY(0, 0, theta);
  ROS_DEBUG("theta averaged: %f", particle.pose.theta);
  tf::quaternionTFToMsg(q, odom_msg.pose.pose.orientation);
  return odom_msg;
}

void
publishPoses(const ros::Publisher &posePub,
             const ros::Publisher &posesPub,
             const ros::Publisher &weightsPub,
             const ParticleFilterStateEstimator &pf,
             unsigned long &seq,
             tf::TransformBroadcaster &br,
             const Measurement &z
) {
  auto genericHeader = Header();
  genericHeader.frame_id = MAP_FRAME;
  genericHeader.seq = seq;

  double maxWeight = 0;
  unsigned long maxWeightIndex = 0;
  for (unsigned long i = 0; i < pf.weights.size(); ++i) {
    if (pf.weights[i] > maxWeight) {
      maxWeight = pf.weights[i];
      maxWeightIndex = i;
    }
  }

  nav_msgs::Odometry poseEstimate = smoothPose(pf.particles[maxWeightIndex]);
  posePub.publish(poseEstimate);

  if (posesPub.getNumSubscribers() > 0 || weightsPub.getNumSubscribers() > 0) {
    PoseArray currentPoses;
    currentPoses.poses.reserve(pf.particles.size());
    for (const auto &p : pf.particles) {
      tf::Quaternion q;
      q.setRPY(0, 0, p.pose.theta);
      Pose newPose;
      tf::quaternionTFToMsg(q, newPose.orientation);
      newPose.position.x = p.pose.x;
      newPose.position.y = p.pose.y;
      currentPoses.poses.push_back(newPose);
    }
    currentPoses.header = genericHeader;
    ROS_DEBUG("Publishing %lu particles", currentPoses.poses.size());
    posesPub.publish(currentPoses);

    MarkerArray currentWeights;
    currentWeights.markers.reserve(pf.particles.size());
    unsigned long i;
    for (i = 0; i < pf.particles.size(); ++i) {
      Marker marker;
      marker.header = currentPoses.header;
      marker.id = i;
      marker.pose = currentPoses.poses[i];
      marker.type = Marker::SPHERE;
      marker.ns = "Weights";
      geometry_msgs::Vector3 scale;
      scale.x = 0.025;
      scale.y = 0.025;
      scale.z = 0.025;
      marker.scale = scale;
      ColorRGBA c;
      c.r = maxWeight != 0.0 ? pf.weights[i] / maxWeight : 1.0;
      c.g = maxWeight != 0.0 ? pf.weights[i] / maxWeight : 1.0;
      c.b = maxWeight != 0.0 ? pf.weights[i] / maxWeight : 1.0;
      c.a = 1;
      marker.color = c;
      currentWeights.markers.push_back(marker);
    }
    if (newPose) {
      Marker marker;
      marker.header = genericHeader;
      marker.id = i;
      marker.pose = rvizPose;
      marker.type = Marker::ARROW;
      geometry_msgs::Vector3 scale;
      scale.x = 0.2;
      scale.y = 0.05;
      scale.z = 0.05;
      marker.scale = scale;
      ColorRGBA c;
      c.r = 1.0;
      c.g = 0.7;
      c.b = 0.0;
      c.a = 1;
      marker.color = c;
      currentWeights.markers.push_back(marker);

      marker.id = ++i;
      marker.type = Marker::TEXT_VIEW_FACING;
      double w = pf.measurementModel->run(z, marker.pose, MapHandler::currentMap);
      marker.text = to_string(w);
      marker.scale.z = 0.3;
      currentWeights.markers.push_back(marker);
      newPose = false;

      marker = Marker();
      marker.header = genericHeader;
      marker.type = Marker::LINE_LIST;
      marker.pose = poseEstimate.pose.pose;
      marker.scale.x = 0.01;
      marker.id = ++i;
      marker.ns = "Ranges";
      marker.color.a = 1.0;
      marker.color.r = 0.1;
      marker.color.g = 0.2;
      marker.color.b = 0.8;
      marker.points.clear();
      tf::Quaternion laserRot;
      laserRot.setRPY(0, 0, z.laserPose.theta);
      tf::quaternionTFToMsg(laserRot, marker.pose.orientation);
      marker.pose.position.x = z.laserPose.x;
      marker.pose.position.y = z.laserPose.y;
      Point p0;
      p0.x = 0;
      p0.y = 0;
      for (const auto &z_k : z.data) {
        marker.points.push_back(p0);
        Point p1;
        double theta = z_k.angle;
        double hitX = z_k.range * cos(theta);
        double hitY = z_k.range * sin(theta);
        p1.x = hitX;
        p1.y = hitY;
        marker.points.push_back(p1);
      }
      currentWeights.markers.push_back(marker);

      weightsPub.publish(currentWeights);
    }
  }

  seq = seq + 1;

  tf::Transform transform;
  transform.setOrigin(
      tf::Vector3(
          poseEstimate.pose.pose.position.x,
          poseEstimate.pose.pose.position.y,
          poseEstimate.pose.pose.position.z));
  tf::Quaternion quat;
  quat.setValue(poseEstimate.pose.pose.orientation.x,
                poseEstimate.pose.pose.orientation.y,
                poseEstimate.pose.pose.orientation.z,
                poseEstimate.pose.pose.orientation.w);
  transform.setRotation(quat);
  br.sendTransform(tf::StampedTransform(transform, ros::Time::now(), MAP_FRAME, ROBOT_FRAME));
}

int main(int argc, char **argv) {
  ros::init(argc, argv, "localization");
  ros::NodeHandle n("~");
  ROS_INFO("Localization: Starting...");
  ScanProcessor scanProcessor(360);
  ros::Subscriber scanSub = n.subscribe("/scan", 1, &ScanProcessor::recCallback, &scanProcessor);
  ros::Subscriber initialMapSub = n.subscribe("/lf", 1, MapHandler::recCallback);
  ros::Subscriber odomSub = n.subscribe("/odom", 1, odomCallback);
  ros::Publisher posesPub = n.advertise<PoseArray>("particles", 2);
  ros::Publisher posePub = n.advertise<Odometry>("pose", 2);
  ros::Publisher weightsPub = n.advertise<MarkerArray>("weights", 2);
  ros::Subscriber rvizPoseSub = n.subscribe("/rviz_pose", 2, rvizPoseCallback);
  ros::Rate loopRate(10);

  tf::TransformBroadcaster br;

  double particleCountD;
  bool pc = n.param<double>("particle_count", particleCountD, 64.0);
  auto particleCount = (unsigned long) abs(particleCountD);

  double sigmaHit;
  bool sh = n.param<double>("sigma_hit", sigmaHit, 0.2);
  double lambdaShort;
  bool ls = n.param<double>("lambda_short", lambdaShort, 1.0);
  LidarMeasurementModel measurementModel(0.94, 0.03, 0.01, 0.02, sigmaHit, lambdaShort);

  double sigmaRot;
  bool sr = n.param<double>("sigma_rot", sigmaRot, 0.014);
  double sigmaTra;
  bool st = n.param<double>("sigma_tra", sigmaTra, 0.005);
  OdometryMotionModel motionModel(sigmaRot, sigmaTra);

  // Get starting position
  double init_x, init_y, init_theta;
  n.param<double>("init_x", init_x, 2.1);
  n.param<double>("init_y", init_y, 0.65);
  n.param<double>("init_theta", init_theta, 0.0);
  SimplePose start_pose = {init_x, init_y, init_theta};

  ParticleFilterStateEstimator pf = ParticleFilterStateEstimator(&measurementModel, &motionModel, particleCount);

  // Warnings
  if (!pc)
    ROS_WARN("Localization: particle_count not defined, using default");
  if (!sh)
    ROS_WARN("Localization: sigma_hit not defined, using default");
  if (!ls)
    ROS_WARN("Localization: lambda_short not defined, using default");
  if (!sr)
    ROS_WARN("Localization: sigma_rot not defined, using default");
  if (!st)
    ROS_WARN("Localization: sigma_tra not defined, using default");

  unsigned long seq = 0;
  ROS_INFO("Localization: Waiting for valid map");
  while (ros::ok()) {
    ros::spinOnce();
    if (MapHandler::currentMap.valid)
      break;
    loopRate.sleep();
  }
  ROS_INFO("Localization: Map loaded");

  pf.initialiseParticles(MapHandler::currentMap, start_pose);
  publishPoses(posePub, posesPub, weightsPub, pf, seq, br, Measurement());

  ROS_INFO("Localization: Starting particle filter");
  while (ros::ok()) {
    ros::spinOnce();
    if (newPose) {
      pf.initialiseParticles(MapHandler::currentMap, SimplePose(rvizPose));
      newPose = false;
    }
    if (MapHandler::currentMap.valid && newAction) {
      ROS_DEBUG("New Action");
      Action action = getAction();
      ROS_DEBUG("Rotation: %f", action.rot);
      ROS_DEBUG("Translation: %f", action.trans);

      ROS_DEBUG("Filter has %lu particles", pf.particles.size());
      pf.actionUpdate(action);

      auto z = scanProcessor.getMeasurement();
      if (z.data.empty()) {
        ROS_WARN("Localization: Couldn't get lidar scan. Trying again...");
        ros::Duration(1).sleep();
        continue;
      }
      pf.measurementUpdate(z, MapHandler::currentMap);

      if (rand() < RAND_MAX / 4.0 && shouldResample(action)) {
        pf.particleUpdate();
      }
      publishPoses(posePub, posesPub, weightsPub, pf, seq, br, z);

    }
    loopRate.sleep();
  }

  return 0;
}