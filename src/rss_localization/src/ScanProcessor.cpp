#include "rss_grid_localization/ScanProcessor.h"
#include "rss_grid_localization/util.h"
#include "tf/tf.h"
#include "tf/exceptions.h"
#include "sensor_msgs/LaserScan.h"

using namespace sensor_msgs;
using namespace std;

namespace rss {

Measurement ScanProcessor::getMeasurement() {
  if (currentScan.ranges.empty()) {
    ROS_WARN("Localization: Couldn't get lidar scan. Trying again...");
    ros::Duration(1).sleep();
    return {};
  }
  vector<RangeAnglePair> ranges;
  for (unsigned int i = 0; i < rayCount; i += 3) {
    if (!useBadDataValue || currentScan.ranges[i] != badDataValue) {
      if (currentScan.ranges[i] > 0.175    // 0.175 is to remove the hits inside the robot itself
          && currentScan.ranges[i] < currentScan.range_max) {
        ranges.push_back({currentScan.ranges[i], angles[i]});
      }
    }
  }
  return {currentScan.header, ranges, laserCenter};
}

void ScanProcessor::recCallback(const LaserScan::ConstPtr &msg) {
  currentScan = *msg;
}

ScanProcessor::ScanProcessor(unsigned int rayCount) : rayCount(rayCount) {
  while (true) {
    tf::StampedTransform transform;
    try {
      listener.lookupTransform(
          "/base_scan",
          "/base_footprint",
          ros::Time(0),
          transform);
      laserCenter = transform;
      ROS_INFO("Localization: Laser Scanner is offset from /base_footprint by (x:%f,y:%f,theta=%f)",
               laserCenter.x,
               laserCenter.y,
               laserCenter.theta);
      break;
    }
    catch (tf::TransformException &ex) {
      ROS_WARN("%s", ex.what());
      ros::Duration(0.5).sleep();
    }
  }
  angles.reserve(rayCount);
  if (rayCount == 0) {
    // No rays default to 360 rays
    this->rayCount = defaultRayCount;
  }
  double degIncr = 360.0 / this->rayCount;
  for (double currAngle = 0; currAngle < 360; currAngle += degIncr)
    angles.push_back(deg2rad(currAngle));
}

}
